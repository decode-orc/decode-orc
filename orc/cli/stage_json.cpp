/*
 * File:        stage_json.cpp
 * Module:      orc-cli
 * Purpose:     Machine-readable projection of the stage presenter types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "stage_json.h"

#include <stage_ux_strings.h>

#include <optional>
#include <string>
#include <vector>

#include "stage_details.h"

namespace orc {
namespace cli {

namespace {

/**
 * Write a constrained value with the JSON type the parameter declares.
 *
 * The presenter renders every ParameterValue as a string, because that is what
 * a project file stores. A script should not have to parse
 * that string back, so the declared type decides the JSON form here: a boolean
 * default reads as true/false and a numeric one as a number. The numeric token
 * is written verbatim rather than re-rendered, so no digits are lost on the way
 * out.
 *
 * @param json Receives the member.
 * @param name Member name.
 * @param type Declared parameter type.
 * @param text Presenter-rendered value.
 * @param empty_is_absent True where an empty rendering means the value is not
 *                        set at all, so it writes null. False where the empty
 *                        string is itself the value — a string parameter whose
 *                        default is "" starts empty, which is not the same as
 *                        having no default.
 */
void write_typed_value(JsonWriter* json, const std::string& name,
                       orc::ParameterType type, const std::string& text,
                       bool empty_is_absent) {
  if (text.empty() && empty_is_absent) {
    json->member_null(name);
    return;
  }
  switch (type) {
    case orc::ParameterType::BOOL:
      json->member_bool(name, text == "true");
      return;
    case orc::ParameterType::INT32:
    case orc::ParameterType::UINT32:
    case orc::ParameterType::DOUBLE:
      json->member_number(name, text);
      return;
    case orc::ParameterType::STRING:
    case orc::ParameterType::FILE_PATH:
      break;
  }
  json->member(name, text);
}

/// Presenter rendering of an optional constraint, or an empty string when the
/// constraint is not set.
std::string constraint_text(const std::optional<orc::ParameterValue>& value) {
  return value.has_value() ? orc::parameter_util::value_to_string(*value)
                           : std::string();
}

/**
 * Write the allowed values as objects splitting each entry's stored value from
 * its display label.
 *
 * An allowed_strings entry packs both around a unit separator, which is a
 * control character no script should have to know about or cut on. The value is
 * what goes into a project file or `nodes set`, so it gets its own field; the
 * label follows for anything that presents a choice.
 */
void write_allowed_strings(JsonWriter* json,
                           const std::vector<std::string>& entries) {
  json->key("allowed_strings");
  json->begin_array();
  for (const auto& entry : entries) {
    const auto separator = entry.find(orc::stage_ux::kComboValueLabelSeparator);
    const std::string value =
        separator == std::string::npos ? entry : entry.substr(0, separator);
    const std::string label =
        separator == std::string::npos ? entry : entry.substr(separator + 1);
    json->begin_object();
    json->member("value", value);
    json->member("label", label);
    json->end_object();
  }
  json->end_array();
}

}  // namespace

void write_stage_fields_json(JsonWriter* json,
                             const orc::presenters::StageInfo& stage) {
  json->member("name", stage.name);
  json->member("display_name", stage.display_name);
  json->member("description", stage.description);
  // The stage's menu category as the stable id `--kind` takes, not the word
  // the Add Stage menu heads its section with.
  json->member("kind", orc::presenters::stageKindId(stage));
  json->member_bool("is_source", stage.is_source);
  json->member_bool("is_sink", stage.is_sink);
  json->member_bool("is_core_plugin", stage.is_core_plugin);
  json->member("owning_plugin_id", stage.owning_plugin_id);
}

void write_stage_parameter_json(JsonWriter* json,
                                const orc::ParameterDescriptor& descriptor) {
  json->begin_object();
  json->member("name", descriptor.name);
  json->member("display_name", descriptor.display_name);
  json->member("description", descriptor.description);
  json->member("type", orc::parameter_util::type_name(descriptor.type));

  json->key("constraints");
  json->begin_object();
  write_typed_value(json, "min_value", descriptor.type,
                    constraint_text(descriptor.constraints.min_value),
                    /*empty_is_absent=*/true);
  write_typed_value(json, "max_value", descriptor.type,
                    constraint_text(descriptor.constraints.max_value),
                    /*empty_is_absent=*/true);
  // The stored default, including the type-appropriate implicit one — the same
  // value a project file holds and `nodes reset` writes.
  write_typed_value(json, "default_value", descriptor.type,
                    orc::presenters::stageParameterStoredDefault(descriptor),
                    /*empty_is_absent=*/false);
  write_allowed_strings(json, descriptor.constraints.allowed_strings);
  json->member_bool("required", descriptor.constraints.required);
  if (descriptor.constraints.depends_on.has_value()) {
    const auto& dependency = *descriptor.constraints.depends_on;
    json->key("depends_on");
    json->begin_object();
    json->member("parameter_name", dependency.parameter_name);
    json->member_strings("required_values", dependency.required_values);
    json->member_bool("hide_when_disabled", dependency.hide_when_disabled);
    json->end_object();
  } else {
    json->member_null("depends_on");
  }
  json->end_object();

  json->member("file_extension_hint", descriptor.file_extension_hint);
  json->member_bool("output_path", descriptor.output_path);
  json->end_object();
}

}  // namespace cli
}  // namespace orc
