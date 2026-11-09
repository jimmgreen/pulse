#include "developer_tools.h"
#include "developer_contract.h"
#include "manifest.h"
#include "templates.generated.h"

namespace pulse::extensions {
namespace {
using app::ai::ToolOutcome;
ToolOutcome Invalid(const char* message) {
    return ToolOutcome::Fail("invalid_args", message);
}
}

app::ai::ToolOutcome DescribeExtensionApiTool(const app::ai::ToolCall& call) {
    if (call.cancelled.load()) return ToolOutcome::Fail("cancelled", "The call was cancelled.");
    if (!call.args.is_object() || call.args.size() != 0)
        return Invalid("describe_extension_api requires an empty arguments object.");
    ToolOutcome result;
    result.data = DescribeExtensionApi();
    return result;
}

app::ai::ToolOutcome ValidateExtensionManifestTool(const app::ai::ToolCall& call) {
    if (call.cancelled.load()) return ToolOutcome::Fail("cancelled", "The call was cancelled.");
    if (!call.args.is_object()) return Invalid("arguments must be an object.");
    for (const auto& key : call.args.keys())
        if (key != "manifest") return Invalid("Only the manifest argument is accepted.");
    const auto* manifest = call.args.find("manifest");
    if (!manifest || !manifest->is_string()) return Invalid("manifest is required and must be a string.");
    if (manifest->as_string().size() > kMaxManifestBytes)
        return Invalid("manifest must be at most 65536 UTF-8 bytes.");
    ToolOutcome result;
    result.data = ValidateExtensionManifest(call.args);
    return result;
}

app::ai::ToolOutcome GetExtensionTemplateTool(const app::ai::ToolCall& call) {
    if (call.cancelled.load()) return ToolOutcome::Fail("cancelled", "The call was cancelled.");
    if (!call.args.is_object() || call.args.size() != 1)
        return Invalid("get_extension_template requires only the template argument.");
    const auto* name = call.args.find("template");
    if (!name || !name->is_string() || name->as_string().empty() || name->as_string().size() > 64)
        return Invalid("template must be a catalog identifier of at most 64 ASCII bytes, not a path.");
    json::Value catalog;
    if (!json::Parse(generated::kTemplates, catalog) || !catalog.is_object())
        return ToolOutcome::Fail("internal_error", "The embedded template catalog is invalid.");
    const auto* manifest = catalog.find(name->as_string());
    if (!manifest) return Invalid("Unknown template; use the templates from describe_extension_api.");
    const auto text = json::Serialize(*manifest);
    const auto checked = ParseManifest(text);
    if (!checked) return ToolOutcome::Fail("internal_error", "The embedded template does not match the current manifest contract.");
    if (call.cancelled.load()) return ToolOutcome::Fail("cancelled", "The call was cancelled.");
    ToolOutcome result;
    result.data = json::Value::Object();
    result.data.set("template", name->as_string());
    result.data.set("manifest", text);
    result.data.set("validated", true);
    result.data.set("executed", false);
    return result;
}
}
