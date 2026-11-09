#include "../app/extensions/runtime.h"
#include "../app/extensions/package_revision.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pulse::extensions;
struct Environment {
    std::wstring name, previous;
    bool existed = false;
    Environment(const wchar_t* key, const std::filesystem::path& value) : name(key) {
        const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
        if (size) {
            previous.resize(size);
            const DWORD copied = GetEnvironmentVariableW(key, previous.data(), size);
            previous.resize(copied);
            existed = true;
        }
        if (!SetEnvironmentVariableW(key, value.c_str())) throw std::runtime_error("Cannot isolate process environment");
    }
    ~Environment() { SetEnvironmentVariableW(name.c_str(), existed ? previous.c_str() : nullptr); }
};
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("native-package-runtime-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    Fixture() {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot create isolated runtime fixture");
    }
    ~Fixture() { PrepareShutdown(); std::error_code error; std::filesystem::remove_all(root, error); }
};
void Write(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << bytes;
    if (!file) throw std::runtime_error("Cannot write isolated runtime fixture");
}
StartupResult Start(std::initializer_list<std::wstring> args) {
    std::vector<std::wstring> values{L"pulse.exe"};
    values.insert(values.end(), args);
    std::vector<wchar_t*> pointers;
    for (auto& value : values) pointers.push_back(value.data());
    return PrepareStartup(static_cast<int>(pointers.size()), pointers.data());
}
void Store(const std::filesystem::path& root, const char* id) {
    const std::string manifest = std::string(R"({"apiVersion":1,"id":")") + id + R"(","name":"Runtime fixture"})";
    const auto revision = PackageRevision(manifest);
    Write(root / "versions" / (revision + ".json"), manifest);
    Write(root / "registry.json", std::string(R"({"formatVersion":1,"packages":[{"id":")") + id +
        R"(","name":"Runtime fixture","enabled":true,"activeRevision":")" + revision + R"("}]})");
}
pulse::json::Value ReadJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    pulse::json::Value value;
    if (!pulse::json::Parse(bytes, value)) throw std::runtime_error("Cannot parse fixture JSON");
    return value;
}
std::string NativeManifest() {
    return R"({"apiVersion":1,"id":"native.fixture","name":"Native fixture","native":{"version":1,"architecture":"windows-x64","library":"plugin.dll","sha256":")" +
        std::string(64, 'a') + R"("}})";
}
void AddMissingNative(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "binaries");
    const auto manifest = NativeManifest();
    const auto revision = PackageRevision(manifest);
    Write(root / "versions" / (revision + ".json"), manifest);
    auto registry = ReadJson(root / "registry.json");
    auto entry = pulse::json::Value::Object();
    entry.set("id", "native.fixture");
    entry.set("name", "Native fixture");
    entry.set("enabled", true);
    entry.set("activeRevision", revision);
    auto entries = *registry.find("packages");
    entries.push(std::move(entry));
    registry.set("packages", std::move(entries));
    Write(root / "registry.json", pulse::json::Serialize(registry));
}
}
int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* label) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) ++failures;
    };
    try {
        Fixture fixture;
        Environment local(L"LOCALAPPDATA", fixture.root / "local");
        Environment test(L"PULSE_TEST_DATA_DIR", fixture.root / "test");
        auto result = Start({});
        check(!result.exit_requested && !PackageCount() && !PackageStoreMode(), "absent default store preserves no-package startup");
        Store(fixture.root / "local" / "Pulse" / "extensions", "ordinary");
        result = Start({});
        check(!result.exit_requested && PackageStoreMode() && PackageCount() == 1 &&
            PackageManifest(0)->id == "ordinary" && PackageNativePath(0).empty(), "ordinary startup discovers default store without loading a native module");
        const auto ordinary_store = fixture.root / "local" / "Pulse" / "extensions";
        const auto ordinary_status = ordinary_store.parent_path() / "extension-startup-status.json";
        const auto previous = CaptureCallerBinding(0, 4321);
        Write(ordinary_store / "registry.json", "{");
        result = Start({});
        auto status = ReadJson(ordinary_status);
        check(!result.exit_requested && result.exit_code == 0 && !PackageCount() && !PackageStoreMode() &&
            previous && !ValidateCallerBinding(*previous), "broken default registry preserves host startup and clears all package authority");
        check(status.get_string("status") == "disabled" && !status.get_string("error").empty(),
            "broken default registry records a persistent ordinary profile diagnostic");
        result = Start({L"--extension-store", ordinary_store.wstring()});
        check(result.exit_requested && result.exit_code == 2 && !PackageCount(),
            "explicit damaged store preserves strict startup failure");
        Store(ordinary_store, "ordinary");
        result = Start({});
        status = ReadJson(ordinary_status);
        check(!result.exit_requested && PackageCount() == 1 && PackageManifest(0)->id == "ordinary" &&
            status.get_string("status") == "loaded" && status.get_string("error").empty(),
            "repaired ordinary store loads normally and clears stale error status");
        result = Start({L"--test-instance"});
        check(!result.exit_requested && !PackageCount(), "test instance never falls back to ordinary user store");
        Store(fixture.root / "test" / "extensions", "isolated");
        result = Start({L"--test-instance"});
        check(!result.exit_requested && PackageCount() == 1 && PackageManifest(0)->id == "isolated",
            "test startup discovers only isolated extensions directory");
        const auto isolated_store = fixture.root / "test" / "extensions";
        AddMissingNative(isolated_store);
        result = Start({L"--test-instance"});
        status = ReadJson(isolated_store.parent_path() / "extension-startup-status.json");
        check(!result.exit_requested && !PackageCount() && !PackageStoreMode() &&
            status.get_string("status") == "disabled" && !status.get_string("error").empty(),
            "missing enabled native DLL disables entire default transaction and records isolated diagnostic");
        result = Start({L"--test-instance", L"--extension-store", isolated_store.wstring()});
        check(result.exit_requested && result.exit_code == 2 && !PackageCount(),
            "explicit store still rejects missing enabled native DLL");
        Store(isolated_store, "isolated");
        result = Start({L"--test-instance"});
        status = ReadJson(isolated_store.parent_path() / "extension-startup-status.json");
        check(!result.exit_requested && PackageCount() == 1 && PackageManifest(0)->id == "isolated" &&
            status.get_string("status") == "loaded" && status.get_string("error").empty(),
            "repaired isolated store starts normally after rejected native transaction");
        Store(fixture.root / "explicit", "explicit");
        result = Start({L"--test-instance", L"--extension-store", (fixture.root / "explicit").wstring()});
        check(!result.exit_requested && PackageCount() == 1 && PackageManifest(0)->id == "explicit",
            "explicit store overrides automatic discovery");
        const auto manifest_path = fixture.root / "manifest" / "package.json";
        const std::string native_manifest = NativeManifest();
        Write(manifest_path, native_manifest);
        result = Start({L"--test-instance", L"--extension-manifest", manifest_path.wstring()});
        const auto native_path = (manifest_path.parent_path() / "plugin.dll").wstring();
        check(!result.exit_requested && PackageCount() == 1 && PackageManifest(0)->native &&
            PackageNativePath(0) == native_path, "explicit native manifest resolves image beside its manifest");
        const auto caller = CaptureCallerBinding(0, 12345);
        Write(manifest_path, "{}");
        check(PackageManifest(0) && PackageManifest(0)->native &&
            PackageManifest(0)->native->sha256 == std::string(64, 'a') && PackageNativePath(0) == native_path,
            "manifest file changes never replace the running native snapshot");
        SetPackageEnabled(0, false);
        check(caller && !ValidateCallerBinding(*caller), "disable immediately invalidates native caller binding");
        SetPackageEnabled(0, true);
        const auto replacement = CaptureCallerBinding(0, 12345);
        check(replacement && caller && ValidateCallerBinding(*replacement) && !ValidateCallerBinding(*caller) &&
            PackageNativePath(0) == native_path, "reenable issues fresh binding while preserving startup image path");
        check(PackageNativePath(999).empty(), "unknown package has no native image path");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] fixture: %s\n", error.what());
        return 1;
    }
    return failures ? 1 : 0;
}
