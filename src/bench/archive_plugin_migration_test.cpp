#include "../app/archive_plugin_migration.h"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct EnvironmentGuard {
    std::wstring previous;
    bool present = false;
    EnvironmentGuard() {
        wchar_t value[32768];
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", value, 32768);
        present = count > 0 && count < 32768;
        if (present) previous.assign(value, count);
    }
    ~EnvironmentGuard() {
        SetEnvironmentVariableW(L"LOCALAPPDATA", present ? previous.c_str() : nullptr);
    }
};
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    EnvironmentGuard environment;
    const fs::path root = fs::temp_directory_path() /
        (L"pulse-archive-migration-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) {
        std::cerr << "[FAIL] create isolated fixture directory\n";
        return 1;
    }
    const fs::path archive = root / L"Pulse/packs/archive";
    const fs::path installed = archive / L"installed.json";
    const fs::path marker = archive / L"plugin-migration-notice-v1.seen";
    auto write = [](const fs::path& path) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "legacy-package";
    };
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str())) {
        std::cerr << "[FAIL] redirect LOCALAPPDATA\n";
        fs::remove_all(root);
        return 1;
    }
    check(!pulse::TakeArchivePluginMigrationNotice(), "no package produces no notice");
    write(root / L"Pulse/packs/ffmpeg/installed.json");
    write(root / L"Pulse/packs/archives/installed.json");
    check(!pulse::TakeArchivePluginMigrationNotice(), "other pack directories do not match");
    fs::create_directories(installed);
    check(!pulse::TakeArchivePluginMigrationNotice(), "installed.json directory does not match");
    fs::remove(installed);
    write(installed);
    fs::create_directory(marker);
    check(!pulse::TakeArchivePluginMigrationNotice(), "marker creation failure does not claim notice");
    fs::remove(marker);
    check(pulse::TakeArchivePluginMigrationNotice(), "creation failure remains retryable");
    check(!pulse::TakeArchivePluginMigrationNotice(), "subsequent call does not repeat notice");
    std::ifstream package(installed, std::ios::binary);
    std::string contents;
    package >> contents;
    package.close();
    check(contents == "legacy-package", "legacy installation is preserved");
    fs::remove(marker);
    std::atomic<int> notices{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&] {
            if (pulse::TakeArchivePluginMigrationNotice()) ++notices;
        });
    }
    for (auto& worker : workers) worker.join();
    check(notices == 1, "concurrent callers claim exactly one notice");
    check(!pulse::TakeArchivePluginMigrationNotice(), "completion persists after concurrent callers exit");
    fs::remove(marker);
    HWND window = CreateWindowExW(0, L"STATIC", L"migration-test", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create isolated notification target");
    if (window) {
        pulse::StartArchivePluginMigrationNotice(window);
        MSG message{};
        bool received = false;
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (!received && GetTickCount64() < deadline) {
            received = PeekMessageW(&message, window, pulse::ArchivePluginMigrationNoticeMessage(),
                pulse::ArchivePluginMigrationNoticeMessage(), PM_REMOVE) != FALSE;
            if (!received) Sleep(5);
        }
        check(received && pulse::IsArchivePluginMigrationNoticeTarget(window, message.wParam),
            "background check posts authenticated notice to original window");
        check(!pulse::IsArchivePluginMigrationNoticeTarget(window, 0), "invalid notification token rejected");
        DestroyWindow(window);
        check(!pulse::IsArchivePluginMigrationNoticeTarget(window, message.wParam),
            "destroyed notification target rejected");
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    check(!pulse::TakeArchivePluginMigrationNotice(), "missing LOCALAPPDATA does not use relative paths");
    fs::remove_all(root);
    return failures ? 1 : 0;
}
