#include "../app/ai_assistant_settings_cache.h"
#include "../app/ai_settings.h"
#include <cstdio>
#include <windows.h>
#include <filesystem>
#include <fstream>
#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] %s\n", #x); return 1; } } while (0)
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--fallback") {
        CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", argv[2]));
        bool malformed = false;
        const auto loaded = pulse::app::ai::LoadSettings(&malformed);
        CHECK(!malformed && loaded.enabled == (std::wstring(argv[3]) == L"1"));
        return 0;
    }
    pulse::app::AssistantSettingsFields cache;
    pulse::app::ai::Settings settings;
    struct Provider { const char* id; const wchar_t* url; };
    const std::vector<Provider> providers{{"a", L"https://a"}, {"b", L"https://b"}};
    int reads = 0;
    auto fill = [&](uint64_t now, unsigned epoch) {
        if (!cache.NeedsRefresh(now, epoch)) return;
        ++reads;
        cache.Assign(settings, providers, [](const auto&, const auto& p) { return std::wstring(p.url); });
        cache.at = now;
        cache.epoch = epoch;
    };
    settings.assistant.voice.mic = "first";
    fill(100, 1);
    CHECK(reads == 1 && cache.voice_mic == "first");
    for (int i = 0; i < 1000; ++i) fill(101 + i, 1);
    CHECK(reads == 1);
    CHECK(cache.SetupUrl("a") == L"https://a" && cache.SetupUrl("b") == L"https://b");
    settings.assistant.voice.mic = "external";
    fill(2099, 1);
    CHECK(reads == 1 && cache.voice_mic == "first");
    fill(2100, 1);
    CHECK(reads == 2 && cache.voice_mic == "external");
    settings.assistant.voice.mic = "saved";
    fill(2101, 2);
    CHECK(reads == 3 && cache.voice_mic == "saved");
    settings = {};
    fill(4101, 2);
    CHECK(reads == 4 && cache.voice_mic.empty());
    CHECK(cache.setup_urls.size() == 2 && cache.SetupUrl("a") == L"https://a");
    const auto fixture = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("assistant-cache-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    CHECK(std::filesystem::create_directories(fixture / "Pulse" / "ai"));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{fixture};
    CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", fixture.c_str()));
    CHECK(pulse::app::ai::SettingsPath() == (fixture / "Pulse" / "ai" / "ai.json").wstring());
    auto write = [&](const char* text) {
        std::ofstream stream(fixture / "Pulse" / "ai" / "ai.json", std::ios::binary);
        stream << text;
        return stream.good();
    };
    CHECK(write(R"({"assistant":{"voice":{"mic":"disk-one"}}})"));
    settings = pulse::app::ai::LoadSettings();
    fill(6101, 2);
    CHECK(cache.voice_mic == "disk-one");
    CHECK(write(R"({"assistant":{"voice":{"mic":"disk-two"}}})"));
    settings = pulse::app::ai::LoadSettings();
    fill(6102, 2);
    CHECK(cache.voice_mic == "disk-one");
    fill(8101, 2);
    CHECK(cache.voice_mic == "disk-two");
    CHECK(write("{"));
    bool malformed = false;
    settings = pulse::app::ai::LoadSettings(&malformed);
    CHECK(malformed);
    fill(8102, 3);
    CHECK(cache.voice_mic.empty());
    CHECK(std::filesystem::remove(fixture / "Pulse" / "ai" / "ai.json"));
    settings = pulse::app::ai::LoadSettings(&malformed);
    CHECK(!malformed && settings.assistant.voice.mic.empty());
    const auto isolated_exe = fixture / "fallback-test.exe";
    wchar_t executable[32768]{};
    CHECK(GetModuleFileNameW(nullptr, executable, 32768) > 0);
    CHECK(std::filesystem::copy_file(executable, isolated_exe));
    for (int enabled : {0, 1}) {
        { std::ofstream ini(fixture / "setup-defaults.ini");
          ini << "[features]\nai_assistant=" << enabled << "\n"; CHECK(ini.good()); }
        std::wstring command = L"\"" + isolated_exe.wstring() + L"\" --fallback \"" + fixture.wstring() +
            L"\" " + std::to_wstring(enabled);
        STARTUPINFOW start{sizeof(start)};
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(isolated_exe.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, fixture.c_str(), &start, &process));
        CloseHandle(process.hThread);
        const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        CHECK(waited == WAIT_OBJECT_0 && code == 0);
    }
    std::puts("[PASS] isolated executable setup-defaults.ini fallback disabled/enabled");
    std::puts("[PASS] isolated ai.json external edit, malformed input and deletion");
    std::puts("[PASS] first load, repeated fill, provider selection, 2s boundary, epoch invalidation, default reset");
}
