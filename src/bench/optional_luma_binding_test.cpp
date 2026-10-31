#include <windows.h>
#include <filesystem>
#include <string>
#include <cstdio>
#include <initializer_list>
namespace {
using Probe = BOOL (__cdecl*)(int*);
using Value = int (__cdecl*)();
unsigned checks = 0, failures = 0;
void Check(bool condition, const char* text) {
    ++checks; if (!condition) ++failures;
    std::printf("[%s] %s error=%lu\n", condition ? "PASS" : "FAIL", text, GetLastError());
}
std::wstring Image() {
    std::wstring path(32768, 0);
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count); return path;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 5 && std::wstring(argv[1]) == L"--scenario") {
        const std::wstring scenario = argv[2];
        HMODULE foreign = nullptr;
        if (scenario != L"module-relative") {
            foreign = LoadLibraryExW(argv[4], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            Check(foreign != nullptr, "load owned foreign same-basename fixture first");
        }
        HMODULE fixture = LoadLibraryExW(argv[3], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(fixture != nullptr, "load UI-like fixture from directory different from caller EXE");
        const auto probe = fixture ? reinterpret_cast<Probe>(GetProcAddress(fixture, "PulseLumaBindingProbe")) : nullptr;
        const auto touch = fixture ? reinterpret_cast<Value>(GetProcAddress(fixture, "PulseLumaBindingTouch")) : nullptr;
        Check(probe && touch, "obtain owned fixture entry points");
        if (scenario == L"prebound") Check(touch && touch() == 93, "fixture starts with delay imports bound to foreign module");
        int observed = -1;
        const BOOL available = probe ? probe(&observed) : FALSE;
        if (scenario == L"module-relative") Check(available && observed == 71, "module-relative sibling loads and exact delay binding is usable");
        else {
            Check(!available && observed == 0, "foreign delay module is rejected before renderer export use");
            const auto foreign_value = foreign ? reinterpret_cast<Value>(GetProcAddress(foreign, "PulseLumaFixtureValue")) : nullptr;
            Check(foreign_value && foreign_value() == 93, "preexisting foreign module remains loaded and unchanged");
        }
        // Process exit releases fixture modules; do not unload borrowed imports.
    } else if (argc == 4) {
        const auto root = std::filesystem::absolute(L"bench_data") /
            (L"luma-binding-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        if (!std::filesystem::create_directory(root, error)) return 2;
        const auto ui = root / L"ui", foreign = root / L"foreign";
        if (!std::filesystem::create_directory(ui, error) || !std::filesystem::create_directory(foreign, error)) return 2;
        const auto importer = ui / L"PulseLumaBindingFixture.dll";
        const auto expected_dll = ui / L"lumatext.dll", foreign_dll = foreign / L"lumatext.dll";
        if (!CopyFileW(argv[1], importer.c_str(), TRUE) || !CopyFileW(argv[2], expected_dll.c_str(), TRUE) ||
            !CopyFileW(argv[3], foreign_dll.c_str(), TRUE)) return 2;
        for (const auto* scenario : {L"module-relative", L"collision", L"prebound"}) {
            auto command = L"\"" + Image() + L"\" --scenario " + scenario + L" \"" + importer.wstring() + L"\" \"" + foreign_dll.wstring() + L"\"";
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
            const BOOL launched = CreateProcessW(Image().c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child);
            Check(launched != FALSE, "start fresh owned process for binding scenario");
            if (!launched) continue;
            const DWORD wait = WaitForSingleObject(child.hProcess, 15000);
            DWORD exit_code = STILL_ACTIVE; GetExitCodeProcess(child.hProcess, &exit_code);
            Check(wait == WAIT_OBJECT_0 && exit_code == 0, "binding scenario exits successfully");
            if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 5000); }
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
        }
        Check(DeleteFileW(importer.c_str()) && DeleteFileW(expected_dll.c_str()) && DeleteFileW(foreign_dll.c_str()) &&
            RemoveDirectoryW(ui.c_str()) && RemoveDirectoryW(foreign.c_str()) && RemoveDirectoryW(root.c_str()), "remove owned binding fixture files");
    } else { std::printf("Usage: test absolute-binding-fixture.dll absolute-expected-lumatext.dll absolute-foreign-lumatext.dll\n"); return 2; }
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
