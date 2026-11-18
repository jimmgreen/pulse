#include "../app/extensions/manifest.h"
#include "../common/json_value.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
std::string Wrap(std::string extra = {}) {
    return R"({"apiVersion":1,"id":"sample.native","name":"Sample")" + extra + "}";
}
void Reject(const std::string& input, const char* label) {
    const auto result = pulse::extensions::ParseManifest(input);
    Check(!result && !result.manifest && !result.error.empty(), label);
}
}
int main() {
    using pulse::extensions::ParseManifest;
    const auto metadata = ParseManifest(Wrap());
    Check(metadata && metadata.manifest->id == "sample.native" && metadata.manifest->resources.empty(),
        "native template identity and empty capability declarations preserved");
    const std::string native = R"(,"native":{"version":1,"architecture":"windows-x64","library":"sample.dll","sha256":")" + std::string(64, 'a') + R"("})";
    const auto package = ParseManifest(Wrap(native));
    Check(package && package.manifest->native && package.manifest->native->library == "sample.dll",
        "native DLL package accepted without commands");
    for (const auto* field : {"commands", "toolbar", "palette", "workflows", "panel", "runtime"}) {
        for (const auto* value : {"null", "[]", "{}"}) {
            const auto result = ParseManifest(Wrap(native + ",\"" + field + "\":" + value));
            Check(!result && result.error == std::string(field) + " is no longer supported; use a native C++ DLL package",
                "removed declarations explicitly rejected regardless of value");
        }
    }
    Reject(Wrap(R"(,"commands":[{"id":"run","title":"Run","capability":"runtime.invoke"}])"), "old executable command rejected");
    for (const auto* input : {"[]", "null", "{}", R"({"apiVersion":2,"id":"sample","name":"Sample"})",
        R"({"apiVersion":1,"id":"bad id","name":"Sample"})", R"({"apiVersion":1,"id":"sample","name":" "})",
        R"({"apiVersion":1,"id":"sample","id":"duplicate","name":"Sample"})"}) Reject(input, "invalid metadata rejected");
    Reject(Wrap(R"(,"unknown":true)"), "unknown root field rejected");
    Reject(std::string(pulse::extensions::kMaxManifestBytes + 1, ' '), "manifest byte budget enforced");
    for (const auto* value : {R"(["context.read","context.read"])", R"(["unknown"])", R"([1])", "null"})
        Reject(Wrap(std::string(R"(,"resources":)") + value), "invalid resource declarations rejected");
    const auto resources = ParseManifest(Wrap(R"(,"resources":["context.read","files.transferDirectoryTree","operations.own"])"));
    Check(resources && resources.manifest->resources.size() == 3, "native capability declarations preserved");
    pulse::json::Value base;
    Check(pulse::json::Parse(Wrap(native), base), "native mutation fixture parses");
    for (const auto* library : {"../sample.dll", "C:\\sample.dll", "CON.dll", "LPT1.dll", "sample.exe"}) {
        auto value = base;
        auto entry = *value.find("native"); entry.set("library", library); value.set("native", std::move(entry));
        Reject(pulse::json::Serialize(value), "unsafe native library filename rejected");
    }
    for (const auto* field : {"version", "architecture", "sha256"}) {
        auto value = base;
        auto entry = *value.find("native"); entry.set(field, "invalid"); value.set("native", std::move(entry));
        Reject(pulse::json::Serialize(value), "native ABI and digest constraints preserved");
    }
    const auto examples = std::filesystem::path(PULSE_EXTENSION_EXAMPLES_PATH).parent_path() / "native";
    size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(examples)) {
        const auto path = entry.path() / "manifest.json";
        if (!std::filesystem::is_regular_file(path)) continue;
        std::ifstream input(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        Check(static_cast<bool>(ParseManifest(text)), "native template parses without legacy fields");
        ++checked;
    }
    Check(checked == 8, "all eight current native templates checked");
    return failures ? 1 : 0;
}
