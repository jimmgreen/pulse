#include "setup_file_locks.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pulse::setup;
int ModuleFallback(bool end_owner = false) {
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* text) { ++checks; if (!ok) ++failures; std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n'; };
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"setup-module-locks-" + suffix));
    std::filesystem::create_directories(root);
    wchar_t system[32768]{}; GetSystemDirectoryW(system, 32768);
    const auto file = root / L"version.dll";
    check(CopyFileW((std::filesystem::path(system) / L"version.dll").c_str(), file.c_str(), TRUE) != FALSE,
        "copy DLL only into isolated module fixture");
    const auto original_size = std::filesystem::file_size(file);
    const auto ready_name = L"Local\\PulseModuleReady" + suffix, release_name = L"Local\\PulseModuleRelease" + suffix;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, release_name.c_str());
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --hold-loaded \"" + file.wstring() + L"\" " + ready_name + L" " + release_name;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
    check(started && WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "child loads fixture DLL as a real image");
    const auto report = InspectReplacementLocks(root.wstring(), {L"version.dll"}, true);
    bool identified = false;
    for (const auto& program : report.programs) if (program.pid == child.dwProcessId) identified = true;
    check(!report.Ready() && !report.files.empty(), "mapped DLL is reported even when DELETE access alone is allowed");
    check(identified, "read-only module fallback identifies real DLL owner PID");
    std::cout << "Restart Manager status=" << report.identification_error << ", module_scan_complete=" << report.module_scan_complete << '\n';
    check(std::filesystem::file_size(file) == original_size && started && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT,
        "owner remains running and DLL bytes remain present");
    if (end_owner) {
        bool ended = false;
        for (const auto& program : report.programs) if (program.pid == child.dwProcessId)
            ended = program.CanEnd() && EndLockedProgram(program) == ERROR_SUCCESS;
        check(ended && WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0,
            "identified module owner can be ended using recorded identity");
    }
    SetEvent(release);
    if (started) {
        check(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, end_owner ? "module holder exit completes after explicit action" : "module holder exits voluntarily");
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    check(InspectReplacementLocks(root.wstring(), {L"version.dll"}, true).Ready(), "mapped DLL retry is ready after owner exits");
    CloseHandle(ready); CloseHandle(release);
    check(DeleteFileW(file.c_str()) != FALSE && RemoveDirectoryW(root.c_str()) != FALSE, "remove only isolated module fixture");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring(argv[1]) == L"--inspect") {
        std::vector<std::wstring> files;
        for (int i = 3; i < argc; ++i) files.emplace_back(argv[i]);
        const auto report = InspectReplacementLocks(argv[2], files, true);
        std::wcout << DescribeReplacementLocks(report);
        std::wcout << L"files=" << report.files.size() << L", programs=" << report.programs.size()
            << L", module_scan_complete=" << report.module_scan_complete << L'\n';
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--module-fallback") return ModuleFallback();
    if (argc == 2 && std::wstring(argv[1]) == L"--end-module-owner") return ModuleFallback(true);
    if (argc == 5 && std::wstring(argv[1]) == L"--hold-loaded") {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
        HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        HMODULE module = LoadLibraryW(argv[2]);
        if (!ready || !release || !module) return 2;
        SetEvent(ready); WaitForSingleObject(release, 30000);
        FreeLibrary(module); CloseHandle(ready); CloseHandle(release); return 0;
    }
    if (argc == 5 && std::wstring(argv[1]) == L"--hold") {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
        HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        HANDLE file = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (!ready || !release || file == INVALID_HANDLE_VALUE) return 2;
        SetEvent(ready);
        WaitForSingleObject(release, 30000);
        CloseHandle(file); CloseHandle(ready); CloseHandle(release);
        return 0;
    }
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) { ++checks; if (!ok) ++failures; std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; };
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"setup-locks-" + suffix));
    if (!CreateDirectoryW(root.c_str(), nullptr)) { std::cerr << "Cannot create unique fixture root\n"; return 2; }
    if (!CreateDirectoryW((root / L"DialogBridge").c_str(), nullptr)) { RemoveDirectoryW(root.c_str()); return 2; }
    const auto file = root / L"DialogBridge" / L"pulse.exe";
    { std::ofstream out(file, std::ios::binary); out << "original-component"; }
    const std::wstring ready_name = L"Local\\PulseSetupLockReady" + suffix, release_name = L"Local\\PulseSetupLockRelease" + suffix;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, release_name.c_str());
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --hold \"" + file.wstring() + L"\" " + ready_name + L" " + release_name;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
    check(started, "isolated non-pulse.exe holder starts");
    check(started && WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "holder acquires read-only no-delete lease");
    const auto locked = InspectReplacementLocks(root.wstring(), {L"DialogBridge\\pulse.exe", L"missing.dll"});
    check(!locked.Ready() && locked.error == ERROR_SUCCESS && locked.files.size() == 1 && locked.files[0] == file.wstring(), "nested component lock detected before replacement");
    bool identified = false;
    for (const auto& program : locked.programs) if (program.pid == child.dwProcessId) identified = true;
    check(identified, "Restart Manager identifies external holder PID");
    std::cout << "Restart Manager status=" << locked.identification_error << '\n';
    { std::ifstream in(file, std::ios::binary); std::string contents((std::istreambuf_iterator<char>(in)), {}); check(contents == "original-component", "inspection preserves original bytes while locked"); }
    const bool end_mode = argc == 2 && std::wstring(argv[1]) == L"--end-owner";
    if (end_mode && started) {
        LockedProgram owner{child.dwProcessId, L"isolated holder"};
        FILETIME exited{}, kernel{}, user{};
        check(GetProcessTimes(child.hProcess, &owner.started, &exited, &kernel, &user) != FALSE, "capture exact fixture process identity");
        auto stale = owner; ++stale.started.dwLowDateTime;
        check(EndLockedProgram(stale) == ERROR_INVALID_DATA && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT,
            "stale PID identity leaves live process untouched");
        auto unknown = owner; unknown.started = {};
        check(EndLockedProgram(unknown) == ERROR_ACCESS_DENIED, "unverified identity is not ended");
        auto self = owner; self.pid = GetCurrentProcessId();
        check(EndLockedProgram(self) == ERROR_ACCESS_DENIED, "setup cannot terminate itself");
        check(EndLockedProgram(owner) == ERROR_SUCCESS && WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0,
            "explicit end action terminates only exact isolated owner");
        check(EndLockedProgram(owner) == ERROR_SUCCESS, "already exited owner is idempotent");
    }
    SetEvent(release);
    if (started) { check(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, "holder releases normally"); CloseHandle(child.hThread); CloseHandle(child.hProcess); }
    check(InspectReplacementLocks(root.wstring(), {L"DialogBridge\\pulse.exe", L"missing.dll"}).Ready(), "retry succeeds after release");
    check(InspectReplacementLocks(root.wstring(), {L"..\\outside"}).error == ERROR_INVALID_NAME, "reject escaping payload path");
    { std::ifstream in(file, std::ios::binary); std::string contents((std::istreambuf_iterator<char>(in)), {}); check(contents == "original-component", "successful probe also preserves original bytes"); }
    CloseHandle(ready); CloseHandle(release);
    check(DeleteFileW(file.c_str()) != FALSE, "remove only owned fixture file");
    check(RemoveDirectoryW((root / L"DialogBridge").c_str()) != FALSE && RemoveDirectoryW(root.c_str()) != FALSE, "remove only empty owned fixture directories");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
