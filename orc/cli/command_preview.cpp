/*
 * File:        command_preview.cpp
 * Module:      orc-cli
 * Purpose:     Preview subcommand (outputs / render / compare)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "command_preview.h"

#include <orc_preview_comparison.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cli_exit_codes.h"
#include "frame_numbering.h"
#include "json_writer.h"
#include "project_args.h"
#include "project_presenter.h"
#include "render_presenter.h"

namespace orc {
namespace cli {

namespace {

using orc::presenters::NodeInfo;
using orc::presenters::ProjectPresenter;
using orc::presenters::RenderPresenter;

constexpr uint32_t kMaxScale = 8;
constexpr uint32_t kMaxStretchWindow = 255;
constexpr double kMaxGain = 64.0;
constexpr uint32_t kMaxIntervalMs = 60000;

void print_preview_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name
            << " preview <subcommand> <project> [options]\n";
  std::cerr << "\n";
  std::cerr << "Renders what the preview window shows, without the GUI. A "
               "<node> is a stage's id\n";
  std::cerr << "or its name, as for 'nodes'. Frame numbers start at 1, as the "
               "preview window\n";
  std::cerr << "shows them.\n";
  std::cerr << "\n";
  std::cerr << "Subcommands:\n";
  std::cerr << "  outputs <project> --node <node> [--json]  List the outputs a "
               "stage's preview offers\n";
  std::cerr << "  render <project> --node <node> --frame <N> -o <file.png>  "
               "Write one stage's preview as a PNG\n";
  std::cerr << "  compare <project> --a <node> --b <node> --frame <N> -o "
               "<file.webp>  Write an A/B flicker as an animated WebP\n";
  std::cerr << "\n";
  std::cerr << "Options for 'render' and 'compare':\n";
  std::cerr << "  --output-type TYPE             field1, field2, frame, "
               "frame-reversed, split, luma,\n";
  std::cerr << "                                 chroma or composite "
               "(default: frame)\n";
  std::cerr << "  --option ID                    The output's option id, as "
               "'outputs' lists it\n";
  std::cerr << "                                 (default: the first output "
               "of that type)\n";
  std::cerr << "  --scale K                      Enlarge K times, nearest "
               "neighbour, 1 to 8 (default: 1)\n";
  std::cerr << "  -o, --output FILE              The file to write "
               "(required)\n";
  std::cerr << "\n";
  std::cerr << "Options for 'compare':\n";
  std::cerr << "  --a NODE, --b NODE             The two stages to compare "
               "(required)\n";
  std::cerr << "  --crop x,y,w,h|auto            Cut both pictures to a "
               "rectangle in native samples,\n";
  std::cerr << "                                 or with 'auto' to the window "
               "where A and B differ\n";
  std::cerr << "                                 most (default: the whole "
               "picture)\n";
  std::cerr << "  --crop-size w,h                The window size for --crop "
               "auto (default: 320,240)\n";
  std::cerr << "  --stretch GAIN                 Add a contrast-stretched "
               "copy beside each picture:\n";
  std::cerr << "                                 the difference from A's "
               "local median, times GAIN;\n";
  std::cerr << "                                 0 leaves it out (default: "
               "4)\n";
  std::cerr << "  --stretch-window N             Odd width and height of the "
               "median window, in samples\n";
  std::cerr << "                                 (default: 15)\n";
  std::cerr << "  --interval MS                  How long each of A and B is "
               "shown, 1 to 60000\n";
  std::cerr << "                                 (default: 800)\n";
  std::cerr << "  --diff-output FILE             Also write B - A about mid "
               "grey, times 4, as a PNG,\n";
  std::cerr << "                                 at the same crop and scale "
               "(default: not written)\n";
  std::cerr << "\n";
  std::cerr << "'compare' prints the crop it used as x,y,w,h, so an auto crop "
               "can be repeated\n";
  std::cerr << "exactly with --crop. Each stage's frame N is its own: a stage "
               "that renumbers\n";
  std::cerr << "frames (Frame Map, Source Align, Source Join) on one path only "
               "can make the two\n";
  std::cerr << "show different pictures, and 'compare' warns when the two "
               "frame counts differ.\n";
  std::cerr << "Pictures stay on the native sample grid, without 4:3 aspect "
               "correction, and\n";
  std::cerr << "'compare' never draws dropout highlighting. The animation "
               "loops forever and is\n";
  std::cerr << "lossless, so it can be attached to an issue or pull request "
               "as it is.\n";
  std::cerr << "\n";
  std::cerr << "Scripting:\n";
  std::cerr << "  --json                         Machine-readable output for "
               "'outputs'\n";
  std::cerr << "\n";
  std::cerr << "Examples:\n";
  std::cerr << "  " << program_name
            << " preview outputs disc.orcprj --node \"Video Sink\"\n";
  std::cerr << "  " << program_name
            << " preview render disc.orcprj --node \"Video Sink\" --frame 120 "
               "--scale 2 -o f120.png\n";
  std::cerr << "  " << program_name
            << " preview compare disc.orcprj --a \"Video Sink\" --b \"Video "
               "Sink 2\" --frame 120 \\\n"
               "      --crop auto --scale 2 -o ab.webp\n";
}

// --- Output types -----------------------------------------------------------

std::optional<orc::PreviewOutputType> output_type_from_name(
    const std::string& name) {
  for (const auto& entry : orc::kPreviewOutputTypeCommandNames) {
    if (name == entry.name) {
      return entry.type;
    }
  }
  return std::nullopt;
}

std::string output_type_name(orc::PreviewOutputType type) {
  const char* name = orc::preview_output_type_command_name(type);
  return name != nullptr ? name : "unknown";
}

std::string output_type_names() {
  std::string names;
  for (const auto& entry : orc::kPreviewOutputTypeCommandNames) {
    if (!names.empty()) {
      names += ", ";
    }
    names += entry.name;
  }
  return names;
}

// --- Arguments --------------------------------------------------------------

struct Arguments {
  std::vector<std::string> positional;
  std::map<std::string, std::string> values;
  bool json = false;
};

/// Split argv into positionals and `--name value` options. @p value_options
/// are the options this subcommand takes; -o is --output. Reports and
/// returns false on an unknown, repeated or valueless option.
bool parse_arguments(int argc, char* argv[],
                     const std::set<std::string>& value_options,
                     bool accepts_json, Arguments* parsed) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-o") {
      arg = "--output";
    }
    if (arg == "--json" && accepts_json) {
      parsed->json = true;
      continue;
    }
    if (arg.size() > 1 && arg[0] == '-') {
      if (value_options.count(arg) == 0) {
        std::cerr << "Error: Unknown option: " << arg << "\n";
        return false;
      }
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " needs a value\n";
        return false;
      }
      if (!parsed->values.emplace(arg, argv[++i]).second) {
        std::cerr << "Error: " << arg << " is given more than once\n";
        return false;
      }
      continue;
    }
    parsed->positional.push_back(arg);
  }
  return true;
}

std::optional<std::string> value_of(const Arguments& args,
                                    const std::string& name) {
  const auto it = args.values.find(name);
  if (it == args.values.end()) {
    return std::nullopt;
  }
  return it->second;
}

/// A whole decimal number in [min, max].
std::optional<uint32_t> parse_uint(const std::string& text, uint32_t min,
                                   uint32_t max) {
  if (text.empty() || text.size() > 10 ||
      text.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const uint64_t value = std::stoull(text);
  if (value < min || value > max) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(value);
}

/// A finite number in [0, max].
std::optional<double> parse_gain(const std::string& text, double max) {
  if (text.empty()) {
    return std::nullopt;
  }
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (errno != 0 || end != text.c_str() + text.size() ||
      !std::isfinite(value) || value < 0.0 || value > max) {
    return std::nullopt;
  }
  return value;
}

/// Comma-separated whole numbers, exactly @p count of them.
std::optional<std::vector<uint32_t>> parse_uint_list(const std::string& text,
                                                     size_t count) {
  std::vector<uint32_t> values;
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const auto value =
        parse_uint(text.substr(start, comma - start), 0, UINT32_MAX);
    if (!value) {
      return std::nullopt;
    }
    values.push_back(*value);
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  if (values.size() != count) {
    return std::nullopt;
  }
  return values;
}

/// The options `render` and `compare` share, checked before anything loads.
struct RenderOptions {
  orc::PreviewOutputType type = orc::PreviewOutputType::Frame_Field1_First;
  std::optional<std::string> option_id;
  uint64_t index = 0;
  uint32_t scale = 1;
  std::string output_path;
};

bool parse_render_options(const Arguments& args, RenderOptions* options) {
  if (const auto type = value_of(args, "--output-type")) {
    const auto parsed = output_type_from_name(*type);
    if (!parsed) {
      std::cerr << "Error: Unknown --output-type '" << *type
                << "' (expected one of " << output_type_names() << ")\n";
      return false;
    }
    options->type = *parsed;
  }
  options->option_id = value_of(args, "--option");

  const auto frame = value_of(args, "--frame");
  if (!frame) {
    std::cerr << "Error: --frame is required (frame numbers start at 1)\n";
    return false;
  }
  const auto index = orc::frame_index_from_presentation(*frame);
  if (!index) {
    std::cerr << "Error: --frame must be a frame number from 1, got '" << *frame
              << "'\n";
    return false;
  }
  options->index = *index;

  if (const auto scale = value_of(args, "--scale")) {
    const auto parsed = parse_uint(*scale, 1, kMaxScale);
    if (!parsed) {
      std::cerr << "Error: --scale must be a whole number from 1 to "
                << kMaxScale << ", got '" << *scale << "'\n";
      return false;
    }
    options->scale = *parsed;
  }

  const auto output = value_of(args, "--output");
  if (!output || output->empty()) {
    std::cerr << "Error: -o FILE is required\n";
    return false;
  }
  options->output_path = *output;
  return true;
}

// --- Project and outputs ----------------------------------------------------

/// A render presenter on the project's graph, without the background
/// observation a GUI session runs.
std::unique_ptr<RenderPresenter> make_renderer(ProjectPresenter* presenter) {
  std::shared_ptr<void> dag;
  try {
    dag = presenter->getDAG();
  } catch (const std::exception& e) {
    std::cerr << "Error: The project's graph cannot be built: " << e.what()
              << "\n";
    return nullptr;
  }
  if (!dag) {
    std::cerr << "Error: The project's graph cannot be built\n";
    return nullptr;
  }
  auto renderer =
      std::make_unique<RenderPresenter>(presenter->getCoreProjectHandle());
  renderer->setBackgroundObservationEnabled(false);
  renderer->setDAG(std::move(dag));
  return renderer;
}

std::string node_text(const NodeInfo& node) {
  return "'" + node.label + "' (id " + node.node_id.to_string() + ")";
}

std::string option_text(const std::string& option_id) {
  return option_id.empty() ? "(none)" : option_id;
}

std::vector<orc::PreviewOutputInfo> available_outputs(RenderPresenter* renderer,
                                                      NodeID node) {
  std::vector<orc::PreviewOutputInfo> available;
  for (auto& output : renderer->getAvailableOutputs(node)) {
    if (output.is_available) {
      available.push_back(std::move(output));
    }
  }
  return available;
}

void report_offered(const std::vector<orc::PreviewOutputInfo>& outputs) {
  if (outputs.empty()) {
    std::cerr << "It offers no preview outputs.\n";
    return;
  }
  std::cerr << "It offers:\n";
  for (const auto& output : outputs) {
    std::cerr << "  --output-type " << output_type_name(output.type)
              << " --option " << option_text(output.option_id) << "  ("
              << output.display_name << ")\n";
  }
}

/// The output @p options name at @p node, defaulting the option to the first
/// of the type. Reports what the node offers when it has no such output.
const orc::PreviewOutputInfo* resolve_output(
    const std::vector<orc::PreviewOutputInfo>& outputs, const NodeInfo& node,
    const RenderOptions& options) {
  for (const auto& output : outputs) {
    if (output.type == options.type &&
        (!options.option_id || output.option_id == *options.option_id)) {
      return &output;
    }
  }
  std::cerr << "Error: " << node_text(node) << " has no "
            << output_type_name(options.type) << " output";
  if (options.option_id) {
    std::cerr << " with option '" << *options.option_id << "'";
  }
  std::cerr << "\n";
  report_offered(outputs);
  return nullptr;
}

bool check_frame(const orc::PreviewOutputInfo& output, const NodeInfo& node,
                 uint64_t index) {
  if (index < output.count) {
    return true;
  }
  std::cerr << "Error: Frame " << index + 1 << " is beyond " << node_text(node)
            << ", whose " << output_type_name(output.type) << " output has ";
  if (output.count == 0) {
    std::cerr << "no frames\n";
  } else {
    std::cerr << output.count << " (frames 1 to " << output.count << ")\n";
  }
  return false;
}

// --- outputs ----------------------------------------------------------------

int cmd_preview_outputs(int argc, char* argv[]) {
  Arguments args;
  if (!parse_arguments(argc, argv, {"--node"}, true, &args)) {
    return kExitUsage;
  }
  const auto address = value_of(args, "--node");
  if (args.positional.size() != 1 || !address) {
    std::cerr << "Usage: orc-cli preview outputs <project> --node <node> "
                 "[--json]\n";
    return kExitUsage;
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(args.positional[0], &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, *address);
  if (!node) {
    return kExitNotFound;
  }
  auto renderer = make_renderer(&presenter);
  if (!renderer) {
    return kExitProjectRejected;
  }

  struct Row {
    orc::PreviewOutputInfo info;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  std::vector<Row> rows;
  for (auto& output : available_outputs(renderer.get(), node->node_id)) {
    Row row{std::move(output)};
    // The size is the first frame's: render it, as the preview would.
    if (row.info.count > 0) {
      const auto rendered = renderer->renderPreview(
          node->node_id, row.info.type, 0, row.info.option_id);
      if (rendered.success && rendered.image.is_valid()) {
        row.width = rendered.image.width;
        row.height = rendered.image.height;
      }
    }
    rows.push_back(std::move(row));
  }

  if (args.json) {
    JsonWriter json(std::cout);
    json.begin_object();
    json.key("node");
    json.begin_object();
    json.member_int("id", node->node_id.value());
    json.member("label", node->label);
    json.end_object();
    json.key("outputs");
    json.begin_array();
    for (const auto& row : rows) {
      json.begin_object();
      json.member("type", output_type_name(row.info.type));
      json.member("name", row.info.display_name);
      json.member("option", row.info.option_id);
      json.member_int("count", static_cast<int64_t>(row.info.count));
      if (row.width > 0) {
        json.member_int("width", row.width);
        json.member_int("height", row.height);
      } else {
        json.member_null("width");
        json.member_null("height");
      }
      json.end_object();
    }
    json.end_array();
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  std::cout << "Preview outputs of " << node_text(*node) << " (" << rows.size()
            << "):\n";
  size_t type_width = 4;
  size_t option_width = 6;
  size_t name_width = 4;
  for (const auto& row : rows) {
    type_width = std::max(type_width, output_type_name(row.info.type).size());
    option_width =
        std::max(option_width, option_text(row.info.option_id).size());
    name_width = std::max(name_width, row.info.display_name.size());
  }
  const auto pad = [](const std::string& text, size_t width) {
    return text +
           std::string(std::max(width, text.size()) - text.size() + 2, ' ');
  };
  std::cout << "  " << pad("Type", type_width) << pad("Option", option_width)
            << pad("Name", name_width) << "Frames  Size\n";
  for (const auto& row : rows) {
    std::cout << "  " << pad(output_type_name(row.info.type), type_width)
              << pad(option_text(row.info.option_id), option_width)
              << pad(row.info.display_name, name_width)
              << pad(std::to_string(row.info.count), 6)
              << (row.width > 0 ? std::to_string(row.width) + " x " +
                                      std::to_string(row.height)
                                : std::string("unknown"))
              << "\n";
  }
  return kExitSuccess;
}

// --- render -----------------------------------------------------------------

int cmd_preview_render(int argc, char* argv[]) {
  Arguments args;
  if (!parse_arguments(argc, argv,
                       {"--node", "--frame", "--output-type", "--option",
                        "--scale", "--output"},
                       false, &args)) {
    return kExitUsage;
  }
  const auto address = value_of(args, "--node");
  if (args.positional.size() != 1 || !address) {
    std::cerr << "Usage: orc-cli preview render <project> --node <node> "
                 "--frame <N> -o <file.png> [options]\n";
    return kExitUsage;
  }
  RenderOptions options;
  if (!parse_render_options(args, &options)) {
    return kExitUsage;
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(args.positional[0], &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, *address);
  if (!node) {
    return kExitNotFound;
  }
  auto renderer = make_renderer(&presenter);
  if (!renderer) {
    return kExitProjectRejected;
  }

  const auto outputs = available_outputs(renderer.get(), node->node_id);
  const auto* output = resolve_output(outputs, *node, options);
  if (output == nullptr || !check_frame(*output, *node, options.index)) {
    return kExitUsage;
  }

  const auto rendered = renderer->renderPreview(
      node->node_id, output->type, options.index, output->option_id);
  if (!rendered.success || !rendered.image.is_valid()) {
    std::cerr << "Error: Rendering " << node_text(*node) << " failed";
    if (!rendered.error_message.empty()) {
      std::cerr << ": " << rendered.error_message;
    }
    std::cerr << "\n";
    return kExitUsage;
  }
  const auto problem = RenderPresenter::writePNG(rendered.image, options.scale,
                                                 options.output_path);
  if (!problem.empty()) {
    std::cerr << "Error: " << problem << "\n";
    return kExitUsage;
  }

  std::cout << "Wrote " << options.output_path << ": frame "
            << options.index + 1 << " of " << node_text(*node) << ", "
            << "--output-type " << output_type_name(output->type)
            << " --option " << option_text(output->option_id) << ", "
            << rendered.image.width * options.scale << " x "
            << rendered.image.height * options.scale << "\n";
  return kExitSuccess;
}

// --- compare ----------------------------------------------------------------

bool parse_compare_options(const Arguments& args,
                           orc::PreviewCompareExportSettings* settings) {
  const auto crop = value_of(args, "--crop");
  if (!crop) {
    settings->crop_mode = orc::PreviewCompareCropMode::None;
  } else if (*crop == "auto") {
    settings->crop_mode = orc::PreviewCompareCropMode::Auto;
  } else {
    const auto values = parse_uint_list(*crop, 4);
    if (!values || (*values)[2] == 0 || (*values)[3] == 0) {
      std::cerr << "Error: --crop must be 'auto' or x,y,w,h with w and h "
                   "above 0, got '"
                << *crop << "'\n";
      return false;
    }
    settings->crop_mode = orc::PreviewCompareCropMode::Explicit;
    settings->crop_x = (*values)[0];
    settings->crop_y = (*values)[1];
    settings->crop_width = (*values)[2];
    settings->crop_height = (*values)[3];
  }

  if (const auto size = value_of(args, "--crop-size")) {
    if (settings->crop_mode != orc::PreviewCompareCropMode::Auto) {
      std::cerr << "Error: --crop-size sets the window for --crop auto, "
                   "which was not given\n";
      return false;
    }
    const auto values = parse_uint_list(*size, 2);
    if (!values || (*values)[0] == 0 || (*values)[1] == 0) {
      std::cerr << "Error: --crop-size must be w,h with both above 0, got '"
                << *size << "'\n";
      return false;
    }
    settings->auto_crop_width = (*values)[0];
    settings->auto_crop_height = (*values)[1];
  }

  if (const auto stretch = value_of(args, "--stretch")) {
    const auto gain = parse_gain(*stretch, kMaxGain);
    if (!gain) {
      std::cerr << "Error: --stretch must be a gain from 0 to " << kMaxGain
                << ", got '" << *stretch << "'\n";
      return false;
    }
    settings->stretch = *gain > 0.0;
    if (settings->stretch) {
      settings->stretch_gain = *gain;
    }
  }

  if (const auto window = value_of(args, "--stretch-window")) {
    const auto parsed = parse_uint(*window, 1, kMaxStretchWindow);
    if (!parsed || *parsed % 2 == 0) {
      std::cerr << "Error: --stretch-window must be an odd number from 1 to "
                << kMaxStretchWindow << ", got '" << *window << "'\n";
      return false;
    }
    settings->stretch_window = *parsed;
  }

  if (const auto interval = value_of(args, "--interval")) {
    const auto parsed = parse_uint(*interval, 1, kMaxIntervalMs);
    if (!parsed) {
      std::cerr << "Error: --interval must be from 1 to " << kMaxIntervalMs
                << " milliseconds, got '" << *interval << "'\n";
      return false;
    }
    settings->interval_ms = *parsed;
  }

  if (const auto diff = value_of(args, "--diff-output")) {
    if (diff->empty()) {
      std::cerr << "Error: --diff-output needs a file name\n";
      return false;
    }
    settings->difference_path = *diff;
  }
  return true;
}

int cmd_preview_compare(int argc, char* argv[]) {
  Arguments args;
  if (!parse_arguments(
          argc, argv,
          {"--a", "--b", "--frame", "--output-type", "--option", "--scale",
           "--output", "--crop", "--crop-size", "--stretch", "--stretch-window",
           "--interval", "--diff-output"},
          false, &args)) {
    return kExitUsage;
  }
  const auto address_a = value_of(args, "--a");
  const auto address_b = value_of(args, "--b");
  if (args.positional.size() != 1 || !address_a || !address_b) {
    std::cerr << "Usage: orc-cli preview compare <project> --a <node> --b "
                 "<node> --frame <N> -o <file.webp> [options]\n";
    return kExitUsage;
  }
  RenderOptions options;
  orc::PreviewCompareExportSettings settings;
  if (!parse_render_options(args, &options) ||
      !parse_compare_options(args, &settings)) {
    return kExitUsage;
  }
  settings.scale = options.scale;

  ProjectPresenter presenter;
  if (!load_project_or_report(args.positional[0], &presenter)) {
    return kExitUsage;
  }
  const auto node_a = find_node_or_report(presenter, *address_a);
  if (!node_a) {
    return kExitNotFound;
  }
  const auto node_b = find_node_or_report(presenter, *address_b);
  if (!node_b) {
    return kExitNotFound;
  }
  auto renderer = make_renderer(&presenter);
  if (!renderer) {
    return kExitProjectRejected;
  }

  // Both sides are checked here, so a refusal names frames from 1 and the
  // options to type; the presenter checks them again before it renders.
  const auto outputs_a = available_outputs(renderer.get(), node_a->node_id);
  const auto* output_a = resolve_output(outputs_a, *node_a, options);
  if (output_a == nullptr) {
    return kExitUsage;
  }
  RenderOptions options_b = options;
  options_b.option_id = output_a->option_id;
  const auto outputs_b = available_outputs(renderer.get(), node_b->node_id);
  const auto* output_b = resolve_output(outputs_b, *node_b, options_b);
  if (output_b == nullptr || !check_frame(*output_a, *node_a, options.index) ||
      !check_frame(*output_b, *node_b, options.index)) {
    return kExitUsage;
  }

  orc::PreviewCompareRequest request;
  request.node_a = node_a->node_id;
  request.node_b = node_b->node_id;
  request.output_type = output_a->type;
  request.option_id = output_a->option_id;
  request.index = options.index;

  const auto exported =
      renderer->exportComparison(request, settings, options.output_path);
  for (const auto& warning : exported.warnings) {
    std::cerr << "Warning: " << warning << "\n";
  }
  if (!exported.success) {
    std::cerr << "Error: " << exported.error << "\n";
    return kExitUsage;
  }

  std::cout << "Wrote " << options.output_path << ": A " << node_text(*node_a)
            << ", B " << node_text(*node_b) << ", frame " << options.index + 1
            << ", " << "--output-type " << output_type_name(request.output_type)
            << " --option " << option_text(request.option_id) << "\n";
  std::cout << "  Frames: 2 of " << exported.frame_width << " x "
            << exported.frame_height << ", " << settings.interval_ms
            << " ms each, looping forever\n";
  std::cout << "  Crop:   " << exported.crop_x << "," << exported.crop_y << ","
            << exported.crop_width << "," << exported.crop_height << "\n";
  if (!settings.difference_path.empty()) {
    std::cout << "  Difference: " << settings.difference_path << "\n";
  }
  if (exported.identical) {
    std::cout << "Note: A and B are identical inside the crop; the frames "
                 "differ only in their A and B labels.\n";
  }
  return kExitSuccess;
}

}  // namespace

int preview_command(int argc, char* argv[]) {
  // argv[0] = program name, argv[1] = subcommand
  if (argc < 2) {
    print_preview_usage(argv[0]);
    return kExitUsage;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "--help" || subcommand == "-h") {
    print_preview_usage(argv[0]);
    return kExitSuccess;
  }

  // Project loading and rendering log through the runtime; keep stdout for
  // the command's answer.
  reserve_stdout_for_json();

  if (subcommand == "outputs") {
    return cmd_preview_outputs(argc - 1, argv + 1);
  }
  if (subcommand == "render") {
    return cmd_preview_render(argc - 1, argv + 1);
  }
  if (subcommand == "compare") {
    return cmd_preview_compare(argc - 1, argv + 1);
  }

  std::cerr << "Error: Unknown preview subcommand: " << subcommand << "\n\n";
  print_preview_usage(argv[0]);
  return kExitUsage;
}

}  // namespace cli
}  // namespace orc
