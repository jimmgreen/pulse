#include "../app/extensions/runtime.h"
#include "../app/extensions/development_reload.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>

namespace {
using namespace pulse::extensions;
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
bool Wait(const std::function<bool()>& ready) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    do { if (ready()) return true; Sleep(10); } while (GetTickCount64() < deadline);
    return false;
}
std::string ManifestText(const char* id) {
    return std::string(R"({"apiVersion":1,"id":")") + id +
        R"(","name":"Development test"})";
}
struct Fixture {
    std::filesystem::path root;
    std::filesystem::path path;
    std::wstring previous_profile;
    bool had_profile = false;
    Fixture() {
        wchar_t buffer[32768]{};
        const DWORD count = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", buffer, 32768);
        had_profile = count > 0 && count < 32768;
        if (had_profile) previous_profile = buffer;
        root = std::filesystem::absolute(std::filesystem::path("bench_data") /
            ("extension-reload-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot create isolated fixture");
        path = root / "manifest.json";
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    }
    ~Fixture() {
        StopDevelopmentReload();
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", had_profile ? previous_profile.c_str() : nullptr);
        std::error_code error;
        for (const auto& file : std::filesystem::directory_iterator(root, error)) {
            if (file.is_regular_file(error)) std::filesystem::remove(file.path(), error);
        }
        std::filesystem::remove(root, error);
    }
    void Write(const std::string& bytes) const {
        const auto temporary = root / "manifest-new.json";
        { std::ofstream output(temporary, std::ios::binary); output << bytes; if (!output) throw std::runtime_error("Fixture write failed"); }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Fixture replacement failed");
    }
    StartupResult Start(bool isolated = true) const {
        std::vector<std::wstring> args{L"pulse.exe", L"--extension-dev", path.wstring()};
        if (isolated) args.push_back(L"--test-instance");
        std::vector<wchar_t*> pointers;
        for (auto& arg : args) pointers.push_back(arg.data());
        return PrepareStartup(static_cast<int>(pointers.size()), pointers.data());
    }
};
}

int main() {
    try {
        Fixture fixture;
        fixture.Write(ManifestText("a"));
        Check(fixture.Start(false).exit_requested && !CurrentManifest(), "development refuses non-isolated startup");
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
        Check(fixture.Start().exit_requested && !CurrentManifest(), "development refuses missing profile");
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", (fixture.root / "missing").c_str());
        Check(fixture.Start().exit_requested && !CurrentManifest(), "development refuses nonexistent profile");
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", fixture.root.c_str());
        {
            std::vector<std::wstring> args{L"pulse.exe", L"--extension-dev", L"\\\\not-a-server\\share\\manifest.json", L"--test-instance"};
            std::vector<wchar_t*> pointers;
            for (auto& arg : args) pointers.push_back(arg.data());
            Check(PrepareStartup(static_cast<int>(pointers.size()), pointers.data()).exit_requested && !CurrentManifest(),
                "development refuses UNC manifest before opening network path");
        }
        Check(!fixture.Start().exit_requested && DevelopmentMode() && Enabled() && CurrentManifest()->id == "a",
            "explicit isolated development loads first snapshot");
        Check(StartDevelopmentReload(), "start owned background watcher");
        int revoked = 0;
        const auto epoch = CurrentManifestGeneration();
        const auto caller_before_reload = IssueCaller(0, 1234);
        fixture.Write(ManifestText("b"));
        Check(Wait([&] { return PollDevelopmentReload([&] { Check(CurrentManifest()->id == "a", "old snapshot remains during UI revocation"); ++revoked; }); }) &&
            CurrentManifest()->id == "b" && revoked == 1 && CurrentManifestGeneration() > epoch,
            "valid update revokes old UI before committing new generation");
        const auto caller_after_reload = IssueCaller(0, 1234);
        Check(caller_before_reload && caller_after_reload && !ValidateCaller(*caller_before_reload) &&
            ValidateCaller(*caller_after_reload) && caller_before_reload->package.revision != caller_after_reload->package.revision,
            "background validated replacement revokes caller and publishes new raw-byte revision");
        fixture.Write("{");
        Check(Wait([&] { return DevelopmentStatus().get_string("status") == "invalid"; }), "invalid update exposes machine-readable diagnostic");
        Check(!PollDevelopmentReload([&] { ++revoked; }) && CurrentManifest()->id == "b" && revoked == 1,
            "invalid update retains last valid package and UI");
        SetEnabled(false);
        fixture.Write(ManifestText("c"));
        Check(Wait([&] { return PollDevelopmentReload([&] { ++revoked; }); }) && CurrentManifest()->id == "c" && !Enabled(),
            "valid reload preserves disabled state");
        Check(!Enabled(), "disabled reloaded package remains disabled");
        SetEnabled(true);
        Check(Enabled(), "explicit reenabling permits latest package");
        Check(Wait([&] {
            std::ifstream input(fixture.root / "extension-dev-status.json", std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            pulse::json::Value report;
            return pulse::json::Parse(bytes, report) && report.get_string("status") == "active" && report.get_bool("enabled");
        }), "background worker publishes valid JSON diagnostic after application");
        StopDevelopmentReload();
        fixture.Write(ManifestText("cancelled"));
        Sleep(350);
        Check(!PollDevelopmentReload([&] { ++revoked; }) && CurrentManifest()->id == "c", "stopped watcher cannot publish later changes");

        DevelopmentReload worker;
        Check(worker.Start(41, fixture.path.wstring(), (fixture.root / "worker-status.json").wstring(), ManifestText("cancelled"), true),
            "start isolated revision-race fixture");
        fixture.Write(ManifestText("pending"));
        std::optional<DevelopmentUpdate> old;
        Check(Wait([&] { old = worker.TakeUpdate(); return old && old->manifest.has_value(); }), "take pending valid update");
        fixture.Write("{");
        Check(Wait([&] { return worker.Status().get_string("status") == "invalid"; }), "new invalid revision supersedes taken valid result");
        int swaps = 0;
        Check(old && !worker.Commit(*old, true, [&] { ++swaps; }) && swaps == 0,
            "atomic revision check rejects old valid result after newer invalid revision");
        worker.Stop();
        Check(worker.Start(42, fixture.path.wstring(), (fixture.root / "worker-status.json").wstring(), "{", false), "restart watcher with a new session generation");
        Check(old && !worker.Commit(*old, false, [&] { ++swaps; }) && swaps == 0, "new session rejects prior-generation result");
        fixture.Write(ManifestText("latest"));
        std::optional<DevelopmentUpdate> latest;
        Check(Wait([&] { latest = worker.TakeUpdate(); return latest && latest->manifest.has_value(); }) &&
            worker.Commit(*latest, false, [&] { ++swaps; }) && swaps == 1 && !worker.Status().get_bool("enabled", true),
            "current generation commits once and preserves disabled diagnostic");
        Check(latest && !worker.Commit(*latest, false, [&] { ++swaps; }) && swaps == 1, "already applied result cannot be committed twice");
        worker.Stop();
        Check(!worker.TakeUpdate() && worker.Status().get_string("status") == "stopped", "stop clears pending result and records stopped status");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] development fixture: %s\n", error.what());
        return 1;
    }
    return failures ? 1 : 0;
}
