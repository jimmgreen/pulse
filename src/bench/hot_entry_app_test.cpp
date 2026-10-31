#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <functional>
#include <new>
#include "../app/hot_entry_probe_contract.h"
#include "../app/entry_command_probe_contract.h"
#include "../common/current_user_security.h"

namespace {
namespace hp = pulse::app::hot_entry_probe;
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct View { hp::Shared* value = nullptr; ~View() { if (value) UnmapViewOfFile(value); } };
struct Child { Handle process; DWORD pid = 0; };
std::wstring Canonical(const std::filesystem::path& path) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    std::wstring text(32768, 0);
    const DWORD size = GetFinalPathNameByHandleW(file.value, text.data(), static_cast<DWORD>(text.size()), FILE_NAME_NORMALIZED);
    if (!size || size >= text.size()) return {};
    text.resize(size); return text;
}
bool Same(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool Write(const std::filesystem::path& path, const void* data, DWORD size) {
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    DWORD written = 0;
    return file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, data, size, &written, nullptr) && written == size;
}
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
std::vector<wchar_t> Environment(const std::wstring& fixture, const std::wstring& nonce, int slot) {
    std::vector<std::wstring> entries;
    auto inherited = GetEnvironmentStringsW();
    if (!inherited) return {};
    for (const wchar_t* p = inherited; *p; p += wcslen(p) + 1)
        if (_wcsnicmp(p, L"PULSE_TEST_", 11) != 0) entries.emplace_back(p);
    FreeEnvironmentStringsW(inherited);
    entries.push_back(L"PULSE_TEST_DATA_DIR=" + fixture);
    entries.push_back(L"PULSE_TEST_HOT_ENTRY=" + nonce);
    entries.push_back(L"PULSE_TEST_HOT_SLOT=" + (slot < 0 ? std::wstring(L"primary") : std::to_wstring(slot)));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) { result.insert(result.end(), entry.begin(), entry.end()); result.push_back(0); }
    result.push_back(0); return result;
}
bool Launch(const std::wstring& executable, const std::wstring& operand, const std::filesystem::path& fixture,
    const std::wstring& nonce, int slot, hp::Shared& shared, HANDLE job, Child& child) {
    auto environment = Environment(fixture.wstring(), nonce, slot);
    if (environment.empty()) return false;
    auto command = Quote(executable) + L" " + Quote(operand);
    wchar_t desktop[] = L"Default";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), fixture.c_str(), &startup, &pi)) return false;
    child.process.value = pi.hProcess; child.pid = pi.dwProcessId; Handle thread{pi.hThread};
    BOOL member = FALSE;
    if (!IsProcessInJob(pi.hProcess, job, &member) || (!member && !AssignProcessToJobObject(job, pi.hProcess)) ||
        !IsProcessInJob(pi.hProcess, job, &member) || !member) {
        TerminateProcess(pi.hProcess, 81); WaitForSingleObject(pi.hProcess, 5000); return false;
    }
    if (slot < 0) shared.primary_pid = pi.dwProcessId; else shared.senders[slot].pid = pi.dwProcessId;
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 82); WaitForSingleObject(pi.hProcess, 5000); return false;
    }
    return true;
}
}
int wmain(int argc, wchar_t** argv) {
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!size || size >= ARRAYSIZE(executable) || argc != 2) return 2;
    const auto build = std::filesystem::path(executable).parent_path();
    if (!std::filesystem::path(argv[1]).is_absolute() || !Same(Canonical(argv[1]), Canonical(build / L"pulse.exe"))) return 2;
    const auto bench = build.parent_path() / L"bench_data";
    if (!CreateDirectoryW(bench.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 2;
    GUID guid{}; wchar_t nonce[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, nonce, ARRAYSIZE(nonce))) return 2;
    const auto fixture = bench / (L"hot-entry-" + std::wstring(nonce));
    const auto folder = fixture / L"folder", other = fixture / L"folder-other";
    if (!CreateDirectoryW(fixture.c_str(), nullptr) || !CreateDirectoryW(folder.c_str(), nullptr) || !CreateDirectoryW(other.c_str(), nullptr)) return 2;
    Handle directory{CreateFileW(fixture.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (directory.value == INVALID_HANDLE_VALUE) return 2;
    Handle folder_guard{CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    Handle other_guard{CreateFileW(other.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (folder_guard.value == INVALID_HANDLE_VALUE || other_guard.value == INVALID_HANDLE_VALUE) return 2;
    pulse::app::entry_probe::Contract contract;
    if (wcscpy_s(contract.nonce, nonce) || wcscpy_s(contract.pulse_path, argv[1]) || wcscpy_s(contract.target_path, folder.c_str()) ||
        !Write(fixture / pulse::app::entry_probe::kContractFile, &contract, sizeof(contract))) return 2;
    constexpr char prefs[] = "{\"external_open\":1,\"open_win_e\":1,\"open_pulse_icon\":0,\"open_this_pc\":0,\"new_window_shows\":1,\"auto_check_updates\":false}";
    if (!Write(fixture / L"app.json", prefs, static_cast<DWORD>(sizeof(prefs) - 1))) return 2;
    pulse::CurrentUserSecurityAttributes security;
    if (!security) return 2;
    SetLastError(ERROR_SUCCESS);
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0, sizeof(hp::Shared), hp::MappingName(nonce).c_str())};
    if (!mapping.value || GetLastError() == ERROR_ALREADY_EXISTS) return 2;
    View view{static_cast<hp::Shared*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(hp::Shared)))};
    if (!view.value) return 2;
    auto& shared = *new(view.value) hp::Shared{};
    wcscpy_s(shared.nonce, nonce);
    Handle job{CreateJobObjectW(security.get(), nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 2;
    std::ofstream log(fixture / L"hot-result.tsv", std::ios::binary);
    if (!log) return 2;
    std::wcout << L"FIXTURE\t" << fixture << std::endl;
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) { ++checks; if (!ok) ++failures;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        log << (ok ? "PASS\t" : "FAIL\t") << label << '\n' << std::flush; return ok; };
    Child primary; Child senders[hp::kSenders];
    const auto overall = GetTickCount64() + 90000;
    bool active = check(Launch(argv[1], folder.wstring(), fixture, nonce, -1, shared, job.value, primary), "launch isolated primary without singleton-bypass switch");
    auto wait = [&](const std::function<bool()>& condition, DWORD timeout) {
        const auto until = (std::min)(overall, GetTickCount64() + timeout);
        do {
            if (condition()) return true;
            if (hp::Read(shared.failed) || !primary.process.value || WaitForSingleObject(primary.process.value, 0) != WAIT_TIMEOUT) return false;
            Sleep(20);
        } while (GetTickCount64() < until);
        return condition();
    };
    active = active && check(wait([&] { return hp::Read(shared.ready) != 0; }, 25000), "primary actually presents fixture A with one window/tab");
    auto launch_sender = [&](unsigned slot) {
        const auto operand = slot == 5 ? std::wstring(L"--win-e") : slot == 4 ? other.wstring() : folder.wstring();
        return check(Launch(argv[1], operand, fixture, nonce, static_cast<int>(slot), shared, job.value, senders[slot]), "start real second process in exact private namespace");
    };
    auto completed = [&](unsigned slot) {
        const bool observed = wait([&] { return hp::Read(shared.senders[slot].presented) && WaitForSingleObject(senders[slot].process.value, 0) == WAIT_OBJECT_0; }, 20000);
        DWORD code = STILL_ACTIVE; GetExitCodeProcess(senders[slot].process.value, &code);
        const auto& sender = shared.senders[slot];
        log << "SENDER\t" << slot << "\tpid=" << sender.pid << "\tforwarded=" << sender.forwarded
            << "\tframe=" << hp::Read(shared.senders[slot].presented) << "\twindows=" << sender.presented_windows
            << "\ttabs=" << sender.presented_tabs << "\texit=" << code << "\tid=";
        for (auto byte : sender.attempted_id) log << std::hex << static_cast<unsigned>(byte) << ' ';
        log << std::dec << '\n' << std::flush;
        return check(observed && code == 0 && sender.forwarded && hp::Read(shared.senders[slot].published) &&
            WaitForSingleObject(primary.process.value, 0) == WAIT_TIMEOUT,
            slot == 5 ? "WinE independent tab policy actually presents This PC in third tab" :
            slot == 4 ? "distinct fixture B request dispatched and actually presented" :
            "same-folder request dispatched and actually presented without another window/tab");
    };
    for (unsigned slot = 0; active && slot < 2; ++slot) active = launch_sender(slot) && completed(slot);
    if (active) { active = launch_sender(2) && launch_sender(3); if (active) active = completed(2) && completed(3); }
    for (unsigned slot = 4; active && slot < hp::kSenders; ++slot) active = launch_sender(slot) && completed(slot);
    check(active && !hp::Read(shared.failed) && hp::Read(shared.max_windows) == 1 && hp::Read(shared.max_tabs) == 3,
        "whole run preserves one primary window and exactly three intended tabs");
    InterlockedExchange(&shared.stop, 1);
    bool stopped = primary.process.value && WaitForSingleObject(primary.process.value, 10000) == WAIT_OBJECT_0;
    DWORD primary_exit = STILL_ACTIVE;
    if (primary.process.value) GetExitCodeProcess(primary.process.value, &primary_exit);
    log << "PRIMARY_EXIT\t" << primary_exit << '\n' << std::flush;
    check(stopped && primary_exit == 0, "primary responds to nonce-bound test stop and exits zero");
    if (!stopped) TerminateJobObject(job.value, 83);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{}; bool drained = false;
    const auto drain_until = GetTickCount64() + 5000;
    do {
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) break;
        if (!accounting.ActiveProcesses) { drained = true; break; }
        Sleep(20);
    } while (GetTickCount64() < drain_until);
    check(drained, "only owned job processes have exited");
    log << "LIMIT\tExplicit fixture namespace, real Acquire/Forward/queue and actual frame; no desktop default association or physical WinE claim.\n";
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
