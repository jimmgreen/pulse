#include "../app/extensions/developer_tools.h"
#include "../app/extensions/manifest.h"
#include <atomic>
#include <cstdio>
#include <string>

namespace {
using pulse::app::ai::ToolOutcome;
using pulse::json::Value;
using Tool = ToolOutcome (*)(const pulse::app::ai::ToolCall&);
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
ToolOutcome Call(Tool tool, const Value& args, bool stopped = false) {
    const std::atomic<bool> cancelled{stopped};
    auto result = tool({L"extension developer test", args, cancelled});
    Check(!result.changed_files && result.undo_group == 0, "tool result has no file effects or undo group");
    return result;
}
Value Args(std::string text) {
    auto args = Value::Object();
    args.set("manifest", std::move(text));
    return args;
}
void Reject(Tool tool, const Value& args, const char* label) {
    const auto result = Call(tool, args);
    Check(!result.ok && result.error.code == "invalid_args", label);
}
}

int main() {
    using namespace pulse::extensions;
    const auto empty = Value::Object();
    auto described = Call(DescribeExtensionApiTool, empty);
    const auto* templates = described.data.find("templates");
    Check(templates && templates->is_array() && templates->size() == 20, "discovery lists the shared twenty-template catalog");
    if (templates) for (const auto& name : templates->items()) {
        auto args = Value::Object(); args.set("template", name.as_string());
        auto fetched = Call(GetExtensionTemplateTool, args);
        Check(fetched.ok && fetched.data.get_bool("validated") && !fetched.data.get_bool("executed", true) &&
            fetched.data.get_string("template") == name.as_string() &&
            static_cast<bool>(ParseManifest(fetched.data.get_string("manifest"))),
            "each embedded template is revalidated without execution");
    }
    Reject(GetExtensionTemplateTool, empty, "template retrieval requires an identifier");
    Reject(GetExtensionTemplateTool, Value::Array(), "template retrieval rejects non-object arguments");
    auto unknown_template = Value::Object(); unknown_template.set("template", "../manifest.json");
    Reject(GetExtensionTemplateTool, unknown_template, "template retrieval rejects paths without reading them");
    unknown_template.set("template", 7);
    Reject(GetExtensionTemplateTool, unknown_template, "template retrieval rejects non-string identifiers");
    unknown_template.set("template", "transfer-panel"); unknown_template.set("execute", true);
    Reject(GetExtensionTemplateTool, unknown_template, "template retrieval rejects execution flags");
    Check(Call(GetExtensionTemplateTool, empty, true).error.code == "cancelled", "template retrieval respects cancellation");
    Check(described.ok && described.data.is_object() && described.data.find("manifestSchema"),
        "describe returns manifest JSON schema");
    Reject(DescribeExtensionApiTool, Value::Array(), "describe rejects non-object arguments");
    auto extra = Value::Object();
    extra.set("path", "C:\\not-read.json");
    Reject(DescribeExtensionApiTool, extra, "describe rejects unknown argument");
    const std::string manifest = R"({"apiVersion":1,"id":"developer-test","name":"Developer test","commands":[{"id":"up","title":"Parent folder","capability":"navigation.up"}]})";
    auto result = Call(ValidateExtensionManifestTool, Args(manifest));
    Check(result.ok && result.data.get_bool("valid") && result.data.get_string("error").empty(),
        "valid manifest returns valid true");
    const std::string typed_manifest = R"({"apiVersion":1,"id":"view-test","name":"View","commands":[{"id":"details","title":"Details","capability":"view.setMode","args":{"mode":"details"}}]})";
    result = Call(ValidateExtensionManifestTool, Args(typed_manifest));
    Check(result.ok && result.data.get_bool("valid"), "typed view mode passes developer tool validation");
    const std::string sort_manifest = R"({"apiVersion":1,"id":"sort-test","name":"Sort","commands":[{"id":"all","title":"All","capability":"selection.selectAll"},{"id":"sort","title":"Sort","capability":"view.setSort","args":{"column":"size","direction":"descending"}}]})";
    result = Call(ValidateExtensionManifestTool, Args(sort_manifest));
    Check(result.ok && result.data.get_bool("valid"), "developer tool validates explicit selection and typed sorting without execution");
    for (const auto* invalid_sort : {R"({"column":"size"})", R"({"column":"size","direction":"descending","scope":"global"})", R"({"column":"size","direction":"reverse"})"}) {
        result = Call(ValidateExtensionManifestTool, Args(std::string(R"({"apiVersion":1,"id":"sort-test","name":"Sort","commands":[{"id":"sort","title":"Sort","capability":"view.setSort","args":)") + invalid_sort + "}]}"));
        Check(result.ok && !result.data.get_bool("valid", true), "developer tool rejects invalid sort contract as semantic failure");
    }
    auto invalid_mode = typed_manifest;
    invalid_mode.replace(invalid_mode.find("\"mode\":\"details\""), 16, "\"mode\":\"invalid\"");
    result = Call(ValidateExtensionManifestTool, Args(invalid_mode));
    Check(result.ok && !result.data.get_bool("valid", true), "invalid view mode returns semantic validation failure");
    result = Call(ValidateExtensionManifestTool, Args("{"));
    Check(result.ok && !result.data.get_bool("valid", true) && !result.data.get_string("error").empty(),
        "malformed manifest is a successful validation with valid false");
    result = Call(ValidateExtensionManifestTool, Args(""));
    Check(result.ok && !result.data.get_bool("valid", true), "empty string reaches semantic validation");
    result = Call(ValidateExtensionManifestTool, Args("C:\\not-read.json"));
    Check(result.ok && !result.data.get_bool("valid", true), "path text is validated as JSON and never read as a file");
    Reject(ValidateExtensionManifestTool, Value(), "validate rejects null arguments");
    Reject(ValidateExtensionManifestTool, Value::Array(), "validate rejects array arguments");
    Reject(ValidateExtensionManifestTool, empty, "validate requires manifest argument");
    auto wrong_type = Value::Object();
    wrong_type.set("manifest", 7);
    Reject(ValidateExtensionManifestTool, wrong_type, "validate rejects non-string manifest");
    auto extra_args = Args(manifest);
    extra_args.set("install", true);
    Reject(ValidateExtensionManifestTool, extra_args, "validate rejects unknown install argument");
    std::string boundary = manifest;
    boundary.resize(kMaxManifestBytes, ' ');
    result = Call(ValidateExtensionManifestTool, Args(boundary));
    Check(result.ok && result.data.get_bool("valid"), "exactly 65536 bytes accepted");
    boundary.push_back(' ');
    Reject(ValidateExtensionManifestTool, Args(boundary), "65537 bytes rejected as invalid arguments");
    std::string multibyte = R"({"apiVersion":1,"id":"unicode","name":")";
    multibyte += "\xe4\xb8\xad";
    multibyte += R"(","commands":[]})";
    multibyte.resize(kMaxManifestBytes, ' ');
    result = Call(ValidateExtensionManifestTool, Args(multibyte));
    Check(result.ok && result.data.get_bool("valid"), "UTF-8 manifest accepted at exact byte boundary");
    multibyte.push_back(' ');
    Reject(ValidateExtensionManifestTool, Args(multibyte), "UTF-8 byte boundary enforced rather than character count");
    std::string short_character_count;
    for (int i = 0; i < 22000; ++i) short_character_count += "\xe4\xb8\xad";
    Reject(ValidateExtensionManifestTool, Args(short_character_count), "22000 multibyte characters exceed byte limit");
    result = Call(DescribeExtensionApiTool, empty, true);
    Check(!result.ok && result.error.code == "cancelled", "describe respects cancellation");
    result = Call(ValidateExtensionManifestTool, Args(manifest), true);
    Check(!result.ok && result.error.code == "cancelled", "validate respects cancellation");
    return failures ? 1 : 0;
}
