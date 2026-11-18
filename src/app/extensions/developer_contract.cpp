#include "developer_contract.h"
#include "manifest.h"
#include "api_description.generated.h"
#include <stdexcept>

namespace pulse::extensions {
json::Value DescribeExtensionApi() {
    json::Value result;
    if (!json::Parse(generated::kApiDescription, result) || !result.is_object())
        throw std::logic_error("Invalid generated extension API description");
    return result;
}

json::Value ValidateExtensionManifest(const json::Value& args) {
    auto result = json::Value::Object();
    const auto* manifest = args.find("manifest");
    if (!args.is_object() || args.size() != 1 || !manifest || !manifest->is_string()) {
        result.set("valid", false);
        result.set("error", "Expected exactly {manifest: string} containing manifest JSON text");
        return result;
    }
    const auto parsed = ParseManifest(manifest->as_string());
    result.set("valid", static_cast<bool>(parsed));
    result.set("error", parsed.error);
    return result;
}

}
