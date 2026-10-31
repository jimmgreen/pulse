#include "../common/module_location.h"
#include <filesystem>
#include <string>
#include <cstdio>
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) {
    ++checks; if (!value) ++failures;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
}
std::wstring Image() {
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return count && count < ARRAYSIZE(path) ? std::wstring(path, count) : std::wstring{};
}
bool SameDirectory(const wchar_t* actual, const std::filesystem::path& expected) {
    auto value = expected.wstring();
    if (value.empty() || value.back() != L'\\') value += L'\\';
    return _wcsicmp(actual, value.c_str()) == 0;
}
void Verify(const std::filesystem::path& fixture) {
    const auto own_address = +[] {};
    const void* address = reinterpret_cast<const void*>(own_address);
    wchar_t directory[32768]{};
    const DWORD own = pulse::module_location::DirectoryFromAddress(address, directory, ARRAYSIZE(directory));
    Check(own == ERROR_SUCCESS && SameDirectory(directory, std::filesystem::path(Image()).parent_path()),
        "EXE-owned code resolves its EXE directory");
    Check(pulse::module_location::DirectoryFromAddress(nullptr, directory, ARRAYSIZE(directory)) == ERROR_INVALID_PARAMETER && !directory[0],
        "null code address is rejected without stale output");
    Check(pulse::module_location::DirectoryFromAddress(address, nullptr, 8) == ERROR_INVALID_PARAMETER,
        "null output buffer is rejected");
    wchar_t tiny[2]{L'x', 0};
    Check(pulse::module_location::DirectoryFromAddress(address, tiny, 0) == ERROR_INVALID_PARAMETER,
        "zero capacity is rejected");
    Check(pulse::module_location::DirectoryFromAddress(address, tiny, ARRAYSIZE(tiny)) == ERROR_INSUFFICIENT_BUFFER && !tiny[0],
        "truncated module path is rejected instead of used as a helper root");
    const auto module = LoadLibraryExW(fixture.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(module != nullptr, "load owned fixture DLL from separate directory");
    using Query = DWORD (WINAPI*)(wchar_t*, DWORD);
    const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "PulseModuleDirectory")) : nullptr;
    const DWORD queried = query ? query(directory, ARRAYSIZE(directory)) : ERROR_PROC_NOT_FOUND;
    Check(queried == ERROR_SUCCESS && SameDirectory(directory, fixture.parent_path()) &&
        !SameDirectory(directory, std::filesystem::path(Image()).parent_path()),
        "DLL-owned code resolves DLL directory rather than caller EXE");
    Check(query && query(tiny, ARRAYSIZE(tiny)) == ERROR_INSUFFICIENT_BUFFER && !tiny[0],
        "DLL query preserves small-buffer failure semantics");
    if (module) Check(FreeLibrary(module) != FALSE, "release only this fixture's explicit DLL reference");
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::wstring(argv[1]) == L"--child") {
        wchar_t cwd[32768]{};
        const DWORD count = GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd);
        Check(count && count < ARRAYSIZE(cwd) && _wcsicmp(cwd, argv[3]) == 0,
            "child was launched with a different working directory");
        Verify(std::filesystem::path(argv[2]));
    } else if (argc == 2 && std::filesystem::path(argv[1]).is_absolute()) {
        const auto root = std::filesystem::absolute(L"bench_data") /
            (L"module-location-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const auto module_dir = root / L"dll", cwd = root / L"cwd";
        if (!CreateDirectoryW(root.c_str(), nullptr) || !CreateDirectoryW(module_dir.c_str(), nullptr) ||
            !CreateDirectoryW(cwd.c_str(), nullptr)) return 2;
        const auto fixture = module_dir / L"ModuleLocationFixture.dll";
        if (!CopyFileW(argv[1], fixture.c_str(), TRUE)) return 2;
        Verify(fixture);
        auto command = L"\"" + Image() + L"\" --child \"" + fixture.wstring() + L"\" \"" + cwd.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        const BOOL launched = CreateProcessW(Image().c_str(), command.data(), nullptr, nullptr, TRUE, 0,
            nullptr, cwd.c_str(), &startup, &child);
        Check(launched != FALSE, "launch only owned path-test child with explicit cwd");
        if (launched) {
            const DWORD wait = WaitForSingleObject(child.hProcess, 10000);
            DWORD code = STILL_ACTIVE; GetExitCodeProcess(child.hProcess, &code);
            Check(wait == WAIT_OBJECT_0 && code == 0, "path tests pass independently of caller cwd");
            if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 5000); }
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
        }
        Check(DeleteFileW(fixture.c_str()) && RemoveDirectoryW(module_dir.c_str()) && RemoveDirectoryW(cwd.c_str()) &&
            RemoveDirectoryW(root.c_str()), "owned path-only fixture cleaned");
    } else { std::printf("Usage: test absolute-module-location-fixture.dll\n"); return 2; }
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
