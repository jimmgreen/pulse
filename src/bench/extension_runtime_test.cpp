#include "../app/extensions/runtime.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
pulse::extensions::StartupResult Start(std::initializer_list<std::wstring> arguments) {
    std::vector<std::wstring> storage{L"pulse.exe"};
    storage.insert(storage.end(), arguments);
    std::vector<wchar_t*> pointers;
    for (auto& value : storage) pointers.push_back(value.data());
    return pulse::extensions::PrepareStartup(static_cast<int>(pointers.size()), pointers.data());
}
}
int main() {
    namespace fs = std::filesystem;
    using namespace pulse::extensions;
    const auto root = fs::absolute(fs::path("bench_data") /
        ("extension-runtime-native-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    fs::create_directories(root);
    const auto file = root / "manifest.json";
    const auto legacy = root / "legacy.json";
    std::ofstream(file) << R"({"apiVersion":1,"id":"runtime-test","name":"Runtime test","native":{"version":1,"architecture":"windows-x64","library":"fixture.dll","sha256":")"
        << std::string(64, 'a') << R"("}})";
    std::ofstream(legacy) << R"({"apiVersion":1,"id":"legacy","name":"Legacy","commands":[]})";
    auto started = Start({L"--extension-manifest", file.wstring()});
    Check(!started.exit_requested && Enabled() && PackageCount() == 1 && CurrentManifest()->native.has_value(),
        "native metadata startup creates enabled package without declarative commands");
    Check(PackageNativePath(0) == (root / "fixture.dll").wstring(), "native DLL path bound to manifest directory");
    auto binding = CaptureCallerBinding(0, 12345);
    Check(binding && ValidateCallerBinding(*binding), "live owner receives lifecycle binding");
    SetEnabled(false);
    Check(!Enabled() && binding && !ValidateCallerBinding(*binding), "disable revokes outstanding binding");
    SetEnabled(true);
    Check(Enabled() && binding && !ValidateCallerBinding(*binding), "reenabling does not revive stale binding");
    binding = CaptureCallerBinding(0, 12345);
    ReleaseCallerOwner(12345);
    Check(binding && !ValidateCallerBinding(*binding), "owner teardown revokes binding");
    binding = CaptureCallerBinding(0, 12345);
    const auto rejected = Start({L"--extension-manifest", legacy.wstring()});
    Check(rejected.exit_requested && rejected.exit_code == 2 && !Enabled() && PackageCount() == 0,
        "legacy commands rejected without leaving active package");
    Check(binding && !ValidateCallerBinding(*binding), "failed startup revokes prior generation");
    for (const auto* option : {L"--extension-script-dev", L"--extension-manifest"}) {
        Start({L"--extension-manifest", file.wstring()});
        const auto result = Start({option});
        Check(result.exit_requested && result.exit_code == 2 && PackageCount() == 0,
            "removed script option or missing argument fails with empty runtime");
    }
    Start({L"--extension-manifest", file.wstring()});
    binding = CaptureCallerBinding(0, 12345);
    PrepareShutdown();
    Check(!Enabled() && binding && !ValidateCallerBinding(*binding), "shutdown revokes active work");
    std::error_code error;
    fs::remove(file, error); fs::remove(legacy, error); fs::remove(root, error);
    return failures ? 1 : 0;
}
