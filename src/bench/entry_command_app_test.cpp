#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include "../app/entry_command_probe_contract.h"
#include "../app/shell_integration_registry.h"

namespace {
using pulse::app::entry_probe::Contract;
using pulse::app::entry_probe::Kind;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
std::wstring JobName(const wchar_t* nonce) { return L"Local\\PulseEntryColdJob-" + std::wstring(nonce); }
std::wstring Env(const wchar_t* name) {
    wchar_t text[32768]{};
    const DWORD count = GetEnvironmentVariableW(name, text, ARRAYSIZE(text));
    return count && count < ARRAYSIZE(text) ? text : L"";
}
std::wstring Canonical(const std::filesystem::path& path) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    std::wstring text(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file.value, text.data(), static_cast<DWORD>(text.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= text.size()) return {};
    text.resize(count); return text;
}
bool Exact(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}
bool WriteNew(const std::filesystem::path& path, const void* bytes, DWORD size) {
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    DWORD written = 0;
    return file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, bytes, size, &written, nullptr) && written == size;
}
std::string Text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
std::string NarrowNonce(const wchar_t* nonce) {
    std::string result;
    for (; *nonce; ++nonce) {
        if (*nonce > 127) return {};
        result.push_back(static_cast<char>(*nonce));
    }
    return result;
}
std::wstring Quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'\"'; return result;
}
std::vector<wchar_t> Environment(const std::wstring& fixture, const std::wstring& nonce) {
    std::vector<std::wstring> values;
    if (auto inherited = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1)
            if (_wcsnicmp(entry, L"PULSE_TEST_", 11) != 0) values.emplace_back(entry);
        FreeEnvironmentStringsW(inherited);
    }
    values.push_back(L"PULSE_TEST_DATA_DIR=" + fixture);
    values.push_back(std::wstring(pulse::app::entry_probe::kEnvironment) + L"=" + nonce);
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& value : values) { block.insert(block.end(), value.begin(), value.end()); block.push_back(0); }
    block.push_back(0); return block;
}
bool Spawn(const std::wstring& executable, std::wstring command, const std::filesystem::path& fixture,
           std::vector<wchar_t>& environment, HANDLE job, DWORD& exit_code) {
    wchar_t desktop[] = L"Default";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop;
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), fixture.c_str(), &startup, &child)) {
        std::cerr << "[PROCESS] CreateProcess failed error=" << GetLastError() << std::endl; return false;
    }
    Handle process{child.hProcess}, thread{child.hThread};
    BOOL in_job = FALSE;
    if (!job || !IsProcessInJob(process.value, job, &in_job) ||
        (!in_job && !AssignProcessToJobObject(job, process.value)) ||
        !IsProcessInJob(process.value, job, &in_job) || !in_job) {
        std::cerr << "[PROCESS] assign/verify own job failed error=" << GetLastError() << std::endl;
        TerminateProcess(process.value, 81); WaitForSingleObject(process.value, 5000); return false;
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 82); WaitForSingleObject(process.value, 5000); return false;
    }
    // The driver owns the entire job, including the relay's Pulse child.
    if (WaitForSingleObject(process.value, 45000) != WAIT_OBJECT_0) {
        if (job) { TerminateJobObject(job, 83); WaitForSingleObject(process.value, 5000); }
        return false;
    }
    return GetExitCodeProcess(process.value, &exit_code) != FALSE;
}
std::wstring ReadValue(HKEY root, const std::wstring& key, const wchar_t* name = nullptr, LSTATUS* error = nullptr) {
    DWORD bytes = 0, type = 0;
    auto status = RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, &type, nullptr, &bytes);
    if (error) *error = status;
    if (status != ERROR_SUCCESS) return {};
    if (!bytes || bytes > 65536 || bytes % sizeof(wchar_t)) {
        if (error) *error = ERROR_INVALID_DATA;
        return {};
    }
    const DWORD capacity_bytes = bytes;
    std::wstring result(bytes / sizeof(wchar_t), 0);
    status = RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, &type, result.data(), &bytes);
    if (error) *error = status;
    if (status != ERROR_SUCCESS) return {};
    std::cout << "[REGISTRY_READ] capacity_bytes=" << capacity_bytes << " actual_bytes=" << bytes << std::endl;
    // The sizing query is a capacity estimate; the data query returns the
    // actual byte count. Do not retain zero-filled capacity as part of a value.
    if (!bytes || bytes % sizeof(wchar_t) || bytes / sizeof(wchar_t) > result.size()) {
        if (error) *error = ERROR_INVALID_DATA;
        return {};
    }
    result.resize(bytes / sizeof(wchar_t));
    if (result.back() != 0) { if (error) *error = ERROR_INVALID_DATA; return {}; }
    result.pop_back(); return result;
}
struct ScratchHive {
    HKEY real = nullptr, root = nullptr;
    std::wstring path, nonce;
    std::ofstream diagnostics;
    bool redirected = false, owns = false;
    void Record(const char* stage, LSTATUS status) {
        std::cout << "[REGISTRY] " << stage << " status=" << status << std::endl;
        diagnostics << stage << "\t" << status << '\n' << std::flush;
    }
    void ApplyResult(const char* stage, bool success) {
        const auto failure = pulse::app::LastShellIntegrationFailure();
        Record(stage, success ? ERROR_SUCCESS : failure.status);
        diagnostics << "success=" << success << " failure_kind=" << static_cast<int>(failure.kind)
            << '\n' << std::flush;
        const auto details = std::wstring(stage, stage + strlen(stage)) + L"\t" + failure.key + L"\t" + failure.name + L"\r\n";
        // Hex code units retain exact key/value evidence without locale conversion.
        diagnostics << "key_utf16=" << std::hex;
        for (wchar_t ch : failure.key) diagnostics << static_cast<unsigned>(ch) << ' ';
        diagnostics << "\nname_utf16=";
        for (wchar_t ch : failure.name) diagnostics << static_cast<unsigned>(ch) << ' ';
        diagnostics << std::dec << '\n' << std::flush;
        std::wcerr << details << std::flush;
    }
    bool Open(const std::wstring& value) {
        nonce = value; path = L"Software\\PulseTest\\EntryCommand-" + nonce;
        DWORD disposition = 0;
        auto status = RegOpenCurrentUser(KEY_READ | KEY_WRITE | DELETE, &real);
        Record("open-real-hkcu", status);
        if (status != ERROR_SUCCESS) return false;
        status = RegCreateKeyExW(real, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, &disposition);
        Record("create-scratch", status);
        diagnostics << "disposition=" << disposition << '\n' << std::flush;
        if (status != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) return false;
        status = RegSetValueExW(root, L"FixtureNonce", 0, REG_SZ, reinterpret_cast<const BYTE*>(nonce.c_str()),
            static_cast<DWORD>((nonce.size() + 1) * sizeof(wchar_t)));
        Record("write-marker", status);
        owns = status == ERROR_SUCCESS;
        if (!owns) return false;
        status = RegOverridePredefKey(HKEY_CURRENT_USER, root);
        Record("redirect-hkcu", status);
        if (status != ERROR_SUCCESS) return false;
        redirected = true;
        const auto redirected_marker = ReadValue(HKEY_CURRENT_USER, L"", L"FixtureNonce", &status);
        Record("read-redirect-marker", status);
        const auto owned_marker = ReadValue(root, L"", L"FixtureNonce", &status);
        Record("read-owned-marker", status);
        const bool verified = redirected_marker == nonce && owned_marker == nonce;
        Record("verify-redirect-marker", verified ? ERROR_SUCCESS : ERROR_INVALID_DATA);
        return verified;
    }
    bool Close() {
        bool ok = true;
        if (redirected) {
            const auto status = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            Record("restore-hkcu", status);
            ok = status == ERROR_SUCCESS;
            if (ok) redirected = false;
            else return false; // Retain handles for retry; never delete an active redirected hive.
        }
        if (owns && real && root) {
            LSTATUS marker_status = ERROR_SUCCESS;
            const auto marker = ReadValue(root, L"", L"FixtureNonce", &marker_status);
            Record("read-cleanup-marker", marker_status);
            if (marker_status != ERROR_SUCCESS || marker != nonce) {
                Record("verify-cleanup-marker", ERROR_INVALID_DATA); ok = false;
            } else {
                const auto status = RegDeleteTreeW(root, nullptr);
                Record("clear-owned-scratch-handle", status);
                if (status != ERROR_SUCCESS) ok = false;
                else {
                    RegCloseKey(root); root = nullptr;
                    const auto removed = RegDeleteKeyExW(real, path.c_str(), 0, 0);
                    Record("delete-owned-empty-root", removed);
                    if (removed == ERROR_SUCCESS) owns = false;
                    else ok = false;
                }
            }
        }
        if (root && !owns) { RegCloseKey(root); root = nullptr; }
        if (real && !owns) { RegCloseKey(real); real = nullptr; }
        return ok && !redirected && !owns;
    }
    ~ScratchHive() { Close(); }
};
int Relay(const std::filesystem::path& self, int argc, wchar_t** argv) {
    const auto fixture = self.parent_path();
    const auto data = Canonical(Env(L"PULSE_TEST_DATA_DIR"));
    if (!Exact(Canonical(fixture), data) || fixture.filename().wstring().find(L"entry-command-") != 0) return 2;
    Contract contract;
    Handle input{CreateFileW((fixture / pulse::app::entry_probe::kContractFile).c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER size{}; DWORD read = 0;
    if (input.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(input.value, &size) || size.QuadPart != static_cast<LONGLONG>(sizeof(Contract)) ||
        !ReadFile(input.value, &contract, sizeof(contract), &read, nullptr) || read != sizeof(contract)) return 2;
    if (contract.magic != pulse::app::entry_probe::kMagic || contract.size != sizeof(contract) || contract.reserved ||
        static_cast<uint32_t>(contract.kind) > static_cast<uint32_t>(Kind::WinE) ||
        contract.nonce[39] || contract.pulse_path[4095] || contract.target_path[4095] ||
        (Env(pulse::app::entry_probe::kEnvironment) != contract.nonce)) return 2;
    GUID nonce_guid{};
    if (wcslen(contract.nonce) != 38 || FAILED(CLSIDFromString(contract.nonce, &nonce_guid)) ||
        fixture.filename() != L"entry-command-" + std::wstring(contract.nonce)) return 2;
    const std::wstring expected = contract.kind == Kind::Folder ? contract.target_path :
        contract.kind == Kind::ThisPc ? L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}" :
        contract.kind == Kind::RecycleBin ? L"::{645FF040-5081-101B-9F08-00AA002F954E}" : L"pulse:win-e";
    const bool arguments_match = argc == 3 && std::wstring_view(argv[1]) == L"--shell-open" && argv[2] == expected;
    std::wstring argv_text = L"\xFEFF" L"argc=" + std::to_wstring(argc) + L"\r\n";
    for (int index = 0; index < argc; ++index) argv_text += std::to_wstring(index) + L"\t" + argv[index] + L"\r\n";
    if (!WriteNew(fixture / L"relay-argv.utf16.txt", argv_text.data(), static_cast<DWORD>(argv_text.size() * sizeof(wchar_t)))) return 1;
    const std::string argument_report = (arguments_match ? "PASS\targv\t" : "FAIL\targv\t") + NarrowNonce(contract.nonce) + "\n";
    if (!WriteNew(fixture / L"relay-argv.tsv", argument_report.data(), static_cast<DWORD>(argument_report.size())) || !arguments_match) return 1;
    const auto pulse = Canonical(contract.pulse_path);
    if (!Exact(pulse, Canonical(std::filesystem::path(contract.pulse_path).parent_path() / L"pulse.exe"))) return 2;
    const auto bench = Canonical(std::filesystem::path(contract.pulse_path).parent_path().parent_path() / L"bench_data");
    if (pulse.empty() || bench.empty() || !Exact(Canonical(std::filesystem::path(data).parent_path()), bench) ||
        !Exact(Canonical(contract.target_path), Canonical(fixture / L"folder space 中文 & parentheses()"))) return 2;
    Handle job{OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS | JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE,
        FALSE, JobName(contract.nonce).c_str())};
    BOOL in_job = FALSE;
    if (!job.value || !IsProcessInJob(GetCurrentProcess(), job.value, &in_job) || !in_job) return 2;
    std::wstring command = Quote(contract.pulse_path) + L" --test-instance";
    for (int index = 1; index < argc; ++index) command += L" " + Quote(argv[index]);
    auto environment = Environment(fixture.wstring(), contract.nonce);
    DWORD code = 1;
    return Spawn(contract.pulse_path, command, fixture, environment, job.value, code) && code == 0 ? 0 : 1;
}
}

int wmain(int argc, wchar_t** argv) {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) return 2;
    const std::filesystem::path self(executable);
    if (self.filename() == L"entry relay.exe") return Relay(self, argc, argv);
    const wchar_t* pulse_argument = argc == 2 ? argv[1] : L"";
    if (argc != 2 || !std::filesystem::path(pulse_argument).is_absolute() ||
        !Exact(Canonical(pulse_argument), Canonical(self.parent_path() / L"pulse.exe"))) {
        std::cerr << "Usage: pulse_entry_command_app_test <absolute same-build pulse.exe>\n"; return 2;
    }
    const auto bench = self.parent_path().parent_path() / L"bench_data";
    if (!CreateDirectoryW(bench.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 2;
    bool all = true;
    const pulse::app::ShellIntegrationKind kinds[] = {pulse::app::ShellIntegrationKind::Directory,
        pulse::app::ShellIntegrationKind::ThisPc, pulse::app::ShellIntegrationKind::RecycleBin, pulse::app::ShellIntegrationKind::WinE};
    const wchar_t* keys[] = {L"Software\\Classes\\Directory\\shell\\open\\command",
        L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\open\\command",
        L"Software\\Classes\\CLSID\\{645FF040-5081-101B-9F08-00AA002F954E}\\shell\\open\\command",
        L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command"};
    for (unsigned index = 0; index < static_cast<unsigned>(ARRAYSIZE(kinds)); ++index) {
        GUID guid{}; wchar_t nonce[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, nonce, ARRAYSIZE(nonce))) return 2;
        const auto fixture = bench / (L"entry-command-" + std::wstring(nonce));
        const auto relay = fixture / L"entry relay.exe";
        const auto folder = fixture / L"folder space 中文 & parentheses()";
        if (!CreateDirectoryW(fixture.c_str(), nullptr) || !CreateDirectoryW(folder.c_str(), nullptr) ||
            !CopyFileW(self.c_str(), relay.c_str(), TRUE)) return 2;
        Handle fixture_guard{CreateFileW(fixture.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        if (fixture_guard.value == INVALID_HANDLE_VALUE) return 2;
        Handle relay_guard{CreateFileW(relay.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (relay_guard.value == INVALID_HANDLE_VALUE) return 2;
        Contract contract; contract.kind = static_cast<Kind>(index);
        if (wcslen(pulse_argument) >= ARRAYSIZE(contract.pulse_path) || folder.wstring().size() >= ARRAYSIZE(contract.target_path)) return 2;
        if (wcscpy_s(contract.nonce, nonce) || wcscpy_s(contract.pulse_path, pulse_argument) ||
            wcscpy_s(contract.target_path, folder.c_str()) ||
            !WriteNew(fixture / pulse::app::entry_probe::kContractFile, &contract, sizeof(contract))) return 2;
        std::wstring command;
        ScratchHive hive;
        hive.diagnostics.open(fixture / L"registry-stages.tsv", std::ios::binary);
        if (!hive.diagnostics) return 2;
        hive.diagnostics << "nonce=" << NarrowNonce(nonce) << "\nkind=" << index
            << "\nscratch=Software\\PulseTest\\EntryCommand-" << NarrowNonce(nonce) << '\n' << std::flush;
        const bool opened = hive.Open(nonce);
        bool registered = false;
        if (opened) {
            registered = pulse::app::ApplyShellIntegration(kinds[index], relay.wstring(), true);
            hive.ApplyResult("apply-enable", registered);
            const bool readback = registered && pulse::app::ReadShellIntegration(kinds[index], relay.wstring());
            hive.Record("readback-ownership", readback ? ERROR_SUCCESS : ERROR_INVALID_DATA);
            registered = registered && readback;
            if (registered) {
                LSTATUS status = ERROR_SUCCESS;
                command = ReadValue(HKEY_CURRENT_USER, keys[index], nullptr, &status);
                hive.Record("read-generated-command", status);
            }
            // Never call production restore against real HKCU after a failed
            // remapping. Owned scratch deletion can still proceed separately.
            const bool restored = hive.redirected && ReadValue(HKEY_CURRENT_USER, L"", L"FixtureNonce") == nonce &&
                pulse::app::ApplyShellIntegration(kinds[index], relay.wstring(), false);
            hive.ApplyResult("apply-disable", restored);
            registered = restored && registered;
        }
        const bool cleaned = hive.Close();
        hive.diagnostics << "opened=" << opened << " registered=" << registered << " cleaned=" << cleaned
            << " command_empty=" << command.empty() << '\n' << std::flush;
        if (!opened || !registered || !cleaned || command.empty()) {
            std::cerr << "[FAIL] isolated registry generation/restore, kind=" << index << '\n'; return 1;
        }
        const auto registered_text = L"\xFEFF" + command + L"\r\n";
        if (!WriteNew(fixture / L"registered-command.utf16.txt", registered_text.data(),
            static_cast<DWORD>(registered_text.size() * sizeof(wchar_t)))) return 2;
        if (index == 0) {
            const auto marker = command.find(L"%1");
            if (marker == std::wstring::npos || command.find(L"%1", marker + 2) != std::wstring::npos) return 1;
            command.replace(marker, 2, folder.wstring());
        }
        const auto launched_text = L"\xFEFF" + command + L"\r\n";
        if (!WriteNew(fixture / L"launched-command.utf16.txt", launched_text.data(),
            static_cast<DWORD>(launched_text.size() * sizeof(wchar_t)))) return 2;
        SetLastError(ERROR_SUCCESS);
        Handle job{CreateJobObjectW(nullptr, JobName(nonce).c_str())};
        if (!job.value || GetLastError() == ERROR_ALREADY_EXISTS) return 2;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 2;
        auto environment = Environment(fixture.wstring(), nonce);
        DWORD code = 1;
        const bool ran = Spawn(relay.wstring(), command, fixture, environment, job.value, code);
        const auto expected = "PASS\tpresented\t" + NarrowNonce(nonce) + "\t" + std::to_string(index) + "\n";
        const bool presented = Text(fixture / pulse::app::entry_probe::kPresentationReport).find(expected) != std::string::npos;
        const bool argv_ok = Text(fixture / L"relay-argv.tsv") == "PASS\targv\t" + NarrowNonce(nonce) + "\n";
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        bool drained = false;
        const auto drain_deadline = GetTickCount64() + 5000;
        do {
            if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) break;
            if (!accounting.ActiveProcesses) { drained = true; break; }
            Sleep(20);
        } while (GetTickCount64() < drain_deadline);
        std::cout << "[PROCESS] ran=" << ran << " exit=" << code << " owned_job_drained=" << drained << std::endl;
        const bool pass = ran && code == 0 && argv_ok && presented && drained;
        all = all && pass;
        std::wcout << (pass ? L"[PASS] " : L"[FAIL] ") << L"kind=" << index << L" fixture=" << fixture << L'\n';
    }
    std::cout << "[LIMIT] Generated-command chain, not Windows desktop association or Win+E selection. Four cold launches only.\n";
    return all ? 0 : 1;
}
