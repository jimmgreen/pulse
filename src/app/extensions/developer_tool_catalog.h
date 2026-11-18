#pragma once
#include "templates.generated.h"
#include "../../common/json_value.h"
#include <array>
#include <cstdint>
#include <utility>
#include <string_view>

namespace pulse::extensions {
// Transport-neutral definitions for the read-only authoring tools. Handlers
// still validate arguments; publishing a schema never authorizes execution.
struct DeveloperToolSpec {
    const char* name;
    const char* title;
    const char* description;
    const char* parameter;
    const char* parameter_description;
    int64_t minimum;
    int64_t maximum;
    bool template_choices;
};
inline constexpr std::array<DeveloperToolSpec, 3> kDeveloperTools{{
    {"describe_extension_api", "Describe the extension API",
     "Returns Pulse's declarative extension contract, supported capabilities, limits and manifest JSON schema. "
     "Purely informational: does not read files, load, install or execute an extension.",
     nullptr, nullptr, 0, 0, false},
    {"validate_extension_manifest", "Validate an extension manifest",
     "Validates supplied manifest JSON text against Pulse's extension contract. Returns valid and error. "
     "Does not read files, load, install or execute the manifest. Invalid tool arguments are rejected.",
     "manifest", "Manifest JSON text, at most 65536 UTF-8 bytes (not a file path).", 0, 65536, false},
    {"get_extension_template", "Get an extension template",
     "Returns a built-in declarative template as manifest JSON text, validated against this host. "
     "Use templates from describe_extension_api. Does not read caller files, install, load or execute commands. "
     "Edit the returned data and call validate_extension_manifest before using it. Workflow and resource declarations "
     "require a host-issued package/owner binding at runtime; these developer tools cannot issue that binding, execute "
     "workflows or bypass native per-transfer confirmation.",
     "template", "An exact built-in template identifier, not a path.", 1, 64, true}
}};

inline const DeveloperToolSpec* FindDeveloperTool(std::string_view name) {
    for (const auto& tool : kDeveloperTools) if (name == tool.name) return &tool;
    return nullptr;
}

inline json::Value DeveloperToolInputSchema(const DeveloperToolSpec& tool) {
    auto schema = json::Value::Object();
    schema.set("type", "object");
    auto properties = json::Value::Object();
    if (tool.parameter) {
        auto parameter = json::Value::Object();
        parameter.set("type", "string");
        parameter.set("description", tool.parameter_description);
        parameter.set("minLength", tool.minimum);
        parameter.set("maxLength", tool.maximum);
        if (tool.template_choices) {
            auto choices = json::Value::Array();
            for (const auto* name : generated::kTemplateNames) choices.push(name);
            parameter.set("enum", std::move(choices));
        }
        properties.set(tool.parameter, std::move(parameter));
        auto required = json::Value::Array();
        required.push(tool.parameter);
        schema.set("required", std::move(required));
    }
    schema.set("properties", std::move(properties));
    schema.set("additionalProperties", false);
    return schema;
}

inline json::Value DeveloperFunctionTools() {
    auto tools = json::Value::Array();
    for (const auto& spec : kDeveloperTools) {
        auto function = json::Value::Object();
        function.set("name", spec.name);
        function.set("description", spec.description);
        function.set("parameters", DeveloperToolInputSchema(spec));
        auto tool = json::Value::Object();
        tool.set("type", "function");
        tool.set("function", std::move(function));
        tools.push(std::move(tool));
    }
    return tools;
}
}
