#include "../app/extensions/developer_contract.h"
#include "../app/extensions/developer_tool_catalog.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
}
int main() {
    using namespace pulse;
    using namespace pulse::extensions;
    const auto api = DescribeExtensionApi();
    Check(api.is_object() && api.find("apiVersion"), "native discovery is a valid versioned object");
    const auto* schema = api.find("manifestSchema");
    const auto* properties = schema ? schema->find("properties") : nullptr;
    Check(properties && properties->find("native") && properties->find("resources"), "native package metadata is discoverable");
    for (const auto* field : {"commands", "toolbar", "palette", "workflows", "panel", "runtime"})
        Check(properties && !properties->find(field), "removed declarations absent from manifest discovery");
    Check(api.find("capabilities") && api.find("capabilities")->size() == 0,
        "discovery exposes no declarative executable capabilities");
    Check(api.find("templates") && api.find("templates")->size() == 8,
        "discovery lists the eight native templates");
    Check(DeveloperFunctionTools().size() != 0, "authoring tool discovery remains available");
    auto args = json::Value::Object();
    args.set("manifest", R"({"apiVersion":1,"id":"native","name":"Native"})");
    const auto accepted = ValidateExtensionManifest(args);
    Check(accepted.find("valid") && accepted.find("valid")->as_bool(), "metadata template validates without execution");
    args.set("manifest", R"({"apiVersion":1,"id":"legacy","name":"Legacy","commands":[]})");
    const auto rejected = ValidateExtensionManifest(args);
    Check(rejected.find("valid") && !rejected.find("valid")->as_bool() &&
        rejected.get_string("error").find("commands is no longer supported") != std::string::npos,
        "author validation explains removed commands");
    args.set("unexpected", true);
    const auto malformed = ValidateExtensionManifest(args);
    Check(!malformed.find("valid")->as_bool(), "validation rejects extra call arguments");
    return failures ? 1 : 0;
}
