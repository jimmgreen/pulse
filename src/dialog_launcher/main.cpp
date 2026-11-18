#include "../common/dialog_admin_trust.h"
#include <windows.h>
#include <detours.h>
#include <sddl.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include "../shell_bridge/runtime_config.h"
#include "managed_launch_profile.h"
#include "process_launch_path.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_session.h"

namespace {
namespace fs = std::filesystem;
namespace cfg = pulse::shell_bridge::runtime;
#ifdef _WIN64
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#endif
using pulse::dialog_session::Canonical;
using pulse::dialog_session::AllowedProcess;
using pulse::dialog_session::Machine;
using pulse::dialog_session::ProcessIdentity;
using pulse::dialog_session::Ready;
using pulse::dialog_session::TokenData;
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Local { void* value = nullptr; ~Local() { if (value) LocalFree(value); } };
struct PendingLaunch {
    HANDLE process;
    bool resumed = false;
    ~PendingLaunch() {
        if (!resumed) { TerminateProcess(process, ERROR_INVALID_DATA); WaitForSingleObject(process, 5000); }
    }
};
std::wstring Self() {
    std::wstring value(cfg::kPathUnits, 0);
    const auto count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) return {};
    value.resize(count); return Canonical(value, false);
}
std::wstring Quote(const std::wstring& text) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\'); result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
bool WriteNew(const fs::path& path, const void* data, DWORD bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    return file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, data, bytes, &written, nullptr) &&
        written == bytes && FlushFileBuffers(file.value);
}
bool AnsiPath(const std::wstring& path, std::string& result) {
    // Detours accepts LPCSTR DLL names: reject lossy ACP conversion.
    const bool utf8 = GetACP() == CP_UTF8; BOOL substituted = FALSE;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const int count = WideCharToMultiByte(CP_ACP, flags, path.c_str(), -1, nullptr, 0, nullptr, utf8 ? nullptr : &substituted);
    if (!count || substituted) return false;
    result.resize(static_cast<size_t>(count));
    if (!WideCharToMultiByte(CP_ACP, flags, path.c_str(), -1, result.data(), count, nullptr, utf8 ? nullptr : &substituted) || substituted) return false;
    std::wstring roundtrip(path.size() + 1, 0);
    const int units = MultiByteToWideChar(CP_ACP, 0, result.c_str(), -1, roundtrip.data(), static_cast<int>(roundtrip.size()));
    return units == static_cast<int>(path.size() + 1) && path == roundtrip.c_str();
}
bool Created(HANDLE process, FILETIME& created) { FILETIME exit{}, kernel{}, user{}; return GetProcessTimes(process, &created, &exit, &kernel, &user) != FALSE; }
int Inspect(const fs::path& directory, bool disable) {
    pulse::dialog_session::Status status;
    const DWORD error = pulse::dialog_session::Inspect(directory.wstring(), Self(), disable, status);
    std::cout << "process_alive=" << status.process_alive << " historical_handshake=" << status.historical_handshake
        << " local_disabled=" << status.local_disabled << " error=" << error << " routing_status=not_queried";
    if (status.historical_handshake) std::cout << " initialization=" << std::hex
        << static_cast<unsigned long>(status.initialization) << " entry_coverage=" << status.entry_coverage << std::dec;
    std::cout << "; historical handshake is not current routing or dialog replacement certification.\n";
    if (error) return 2;
    if (disable) {
        std::cout << "Application left running. Active dialogs may finish their existing operation.\n";
        return status.process_alive && status.local_disabled ? 0 : 3;
    }
    return status.process_alive && !status.local_disabled && status.historical_handshake &&
        SUCCEEDED(status.initialization) && status.entry_coverage ? 0 : 3;
}
std::vector<wchar_t> Environment(const cfg::Session& session) {
    std::vector<std::wstring> entries;
    if (auto inherited = GetEnvironmentStringsW()) {
        for (auto* entry = inherited; *entry; entry += wcslen(entry) + 1)
            if (_wcsnicmp(entry, L"PULSE_DIALOG_", 13) != 0) entries.emplace_back(entry);
        FreeEnvironmentStringsW(inherited);
    }
    entries.push_back(L"PULSE_DIALOG_HOST=" + std::wstring(session.host_path));
    entries.push_back(L"PULSE_DIALOG_NONCE=" + std::wstring(session.nonce));
    entries.push_back(L"PULSE_DIALOG_CONTROL=" + std::wstring(session.control_path));
    // Child PID is not known here. Session PID/creation time are written before ResumeThread.
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0); }
    block.push_back(0); return block;
}
std::wstring AdminDrivePath(std::wstring path) {
    // Canonical emits extended drive paths. Do not normalize UNC or devices.
    if (path.size() >= 7 && path.compare(0, 4, LR"(\\?\)") == 0 &&
        ((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) &&
        path[5] == L':' && path[6] == L'\\') path.erase(0, 4);
    return path;
}
int Launch(int argc, wchar_t** argv, pulse::dialog_bundle::BundleLease* verified_bundle = nullptr) {
    std::wstring host, dll, cwd, root, save_profile, bundle_path; int operand = 0; bool admin_session = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--") { operand = i + 1; break; }
        if (arg == L"--admin-session" && !admin_session) { admin_session = true; continue; }
        if (i + 1 == argc) return 2;
        if (arg == L"--bundle" && bundle_path.empty()) bundle_path = argv[++i];
        else if (arg == L"--host") host = argv[++i];
        else if (arg == L"--dll") dll = argv[++i];
        else if (arg == L"--cwd") cwd = argv[++i];
        else if (arg == L"--control-root") root = argv[++i];
        else if (arg == L"--save-profile" && save_profile.empty()) save_profile = argv[++i];
        else return 2;
    }
    if (!operand || operand >= argc) return 2;
    if (admin_session && (!pulse::dialog_admin::IsElevated() || bundle_path.empty() || !save_profile.empty() || verified_bundle || !host.empty() || !dll.empty())) {
        std::cerr << "admin_session_rejected stage=explicit-elevated-bundle error=" << ERROR_ACCESS_DENIED << "\n"; return 2;
    }
    pulse::dialog_bundle::BundleLease selected_bundle;
    if (!bundle_path.empty()) {
        if (!host.empty() || !dll.empty() || verified_bundle) return 2;
        DWORD error = pulse::dialog_bundle::VerifyBundle(AdminDrivePath(bundle_path), {}, selected_bundle);
        auto self = Self(); if (self.starts_with(LR"(\\?\)")) self.erase(0, 4);
        if (!error && (selected_bundle.Machine() != kMachine || _wcsicmp(self.c_str(), selected_bundle.Launcher().c_str()) != 0)) error = ERROR_REVISION_MISMATCH;
        std::cout << "bundle_valid=" << !error << " error=" << error << " process_started=0 runtime_handshake=0\n";
        if (error) return 2;
        host = selected_bundle.Host(); dll = selected_bundle.Runtime(); verified_bundle = &selected_bundle;
    }
    if (admin_session) {
        DWORD error = pulse::dialog_admin::VerifyProtectedTree(AdminDrivePath(selected_bundle.Directory()));
        if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(root), true);
        if (error) { std::cerr << "admin_session_rejected stage=protected-tree-or-control error=" << error << "\n"; return 2; }
    }
    if (!save_profile.empty()) {
        pulse::dialog_launcher::ManagedLaunchProfile profile;
        profile.host = host; profile.runtime = dll; profile.cwd = cwd; profile.control_root = root;
        if (verified_bundle) { profile.format_version = 2; profile.bundle_root = verified_bundle->Directory(); profile.manifest_digest = verified_bundle->ManifestDigest(); }
        profile.target = argv[operand];
        for (int i = operand + 1; i < argc; ++i) profile.arguments.emplace_back(argv[i]);
        auto self = Self(); if (self.starts_with(LR"(\\?\)")) self.erase(0, 4);
        DWORD error = pulse::dialog_launcher::CaptureProfile(profile, self);
        if (!error) error = pulse::dialog_launcher::SaveProfile(save_profile, profile);
        std::cout << "profile_saved=" << (error == ERROR_SUCCESS) << " error=" << error << " process_started=0 runtime_handshake=0\n";
        return error ? 2 : 0;
    }
    const auto target = Canonical(argv[operand], false);
    host = Canonical(host, false); dll = Canonical(dll, false); cwd = Canonical(cwd, true); root = Canonical(root, true);
    WORD target_machine = 0, dll_machine = 0; std::string dll_ansi;
    if (target.empty() || host.empty() || _wcsicmp(fs::path(host).filename().c_str(), L"pulse.exe") != 0 || dll.empty() || cwd.empty() || root.empty() ||
        !Machine(target, target_machine) || !Machine(dll, dll_machine) ||
        !AnsiPath(dll, dll_ansi) || !AllowedProcess(GetCurrentProcess(), admin_session)) {
        std::cerr << "Unsupported paths/architecture/DLL encoding or security context. Requires same-user, same-integrity, unrestricted, medium-or-higher integrity; elevation requires explicit admin-session; no AppContainer/protection/impersonation.\n"; return 2;
    }
    if (target_machine != kMachine || dll_machine != kMachine) {
        std::cerr << "Architecture mismatch: launcher=" << (kMachine == IMAGE_FILE_MACHINE_I386 ? "x86" : "x64")
            << " target_machine=" << target_machine << " runtime_machine=" << dll_machine
            << ". Use the bundle matching the application architecture. process_started=0\n";
        return 2;
    }
    auto session = std::make_unique<cfg::Session>(); session->reserved = admin_session ? cfg::kAdminSession : 0; GUID guid{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, session->nonce, ARRAYSIZE(session->nonce))) return 2;
    const fs::path directory = fs::path(root) / session->nonce;
    std::vector<BYTE> user;
    if (!TokenData(GetCurrentProcess(), TokenUser, user)) return 2;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return 2;
    Local sid_memory{sid}, descriptor;
    const std::wstring sddl = admin_session ? L"O:BAG:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)S:(ML;OICI;NW;;;HI)" : L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;" + std::wstring(sid) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr)) return 2;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.value, FALSE};
    if (!CreateDirectoryW(directory.c_str(), &security)) return 2;
    if (admin_session) {
        const DWORD error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(directory.wstring()), true);
        if (error) { std::cerr << "admin_session_rejected stage=session-acl error=" << error << "\n"; return 2; }
    }
    Handle directory_lock(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (directory_lock.value == INVALID_HANDLE_VALUE) return 2;
    std::ofstream log(directory / L"launcher.log", std::ios::binary);
    if (!log) return 2;
    session->machine = kMachine;
    if (wcscpy_s(session->host_path, host.c_str()) || wcscpy_s(session->target_path, target.c_str()) ||
        wcscpy_s(session->control_path, Canonical(directory, true).c_str()) || wcscpy_s(session->launcher_path, Self().c_str())) return 2;
    const auto launch_target = pulse::dialog_launcher::ProcessLaunchPath(target);
    const auto launch_cwd = pulse::dialog_launcher::ProcessLaunchPath(cwd);
    std::wstring command = Quote(launch_target);
    for (int i = operand + 1; i < argc; ++i) command += L" " + Quote(argv[i]);
    if (command.size() >= 32767) return 2;
    auto environment = Environment(*session);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
    // Explicit target only, no inherited handles, elevation verb or existing-process attachment.
    if (verified_bundle) {
        const DWORD error = pulse::dialog_bundle::RecheckDirectory(*verified_bundle);
        if (error) { std::cout << "bundle_valid=0 error=" << error << " process_started=0 runtime_handshake=0\n"; return 2; }
    }
    const BOOL created = DetourCreateProcessWithDllExW(launch_target.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), launch_cwd.c_str(), &startup, &child, dll_ansi.c_str(), nullptr);
    log << "create=" << created << " error=" << (created ? 0 : GetLastError()) << '\n' << std::flush;
    if (!created) return 1;
    Handle process(child.hProcess), thread(child.hThread);
    PendingLaunch pending{process.value};
    session->target_pid = child.dwProcessId;
    const char* failed_stage = "none";
    DWORD binding_error = ERROR_SUCCESS;
    auto binding_step = [&](const char* name, auto&& action) {
        SetLastError(ERROR_SUCCESS);
        const bool ok = action();
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        log << "binding_stage=" << name << " ok=" << ok << " win32=" << error << '\n' << std::flush;
        if (!ok) { failed_stage = name; binding_error = error; }
        return ok;
    };
    const bool bound =
        binding_step("created", [&] { return Created(process.value, session->target_created); }) &&
        binding_step("allowed_process", [&] { return AllowedProcess(process.value, admin_session); }) &&
        binding_step("process_identity", [&] { return ProcessIdentity(process.value, *session); }) &&
        binding_step("write_session", [&] { return WriteNew(directory / cfg::kSessionFile, session.get(), sizeof(*session)); });
    log << "pid=" << child.dwProcessId << " binding=" << bound << " failed_stage=" << failed_stage
        << " binding_win32=" << binding_error << '\n' << std::flush;
    // Supplemental observations never substitute for the policy result above.
    if (!bound && std::string_view(failed_stage) == "process_identity") {
        FILETIME actual_created{}; SetLastError(ERROR_SUCCESS);
        const bool time_ok = Created(process.value, actual_created);
        const DWORD time_error = time_ok ? ERROR_SUCCESS : GetLastError();
        std::wstring image(cfg::kPathUnits, 0); DWORD units = static_cast<DWORD>(image.size());
        SetLastError(ERROR_SUCCESS);
        const bool image_ok = QueryFullProcessImageNameW(process.value, 0, image.data(), &units) != FALSE;
        const DWORD image_error = image_ok ? ERROR_SUCCESS : GetLastError();
        if (image_ok) image.resize(units); else image.clear();
        const auto canonical = image_ok ? Canonical(image, false) : std::wstring{};
        auto diagnostic_utf8 = [](std::wstring_view value) {
            if (value.empty()) return std::string{};
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (!count) return std::string{"<encoding-error>"};
            std::string result(static_cast<size_t>(count), 0);
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), result.data(), count, nullptr, nullptr))
                return std::string{"<encoding-error>"};
            for (auto& c : result) if (c == '\r' || c == '\n') c = '?';
            return result;
        };
        // Only the explicitly selected executable and this new suspended child.
        log << "identity_expected=" << diagnostic_utf8(session->target_path) << '\n'
            << "identity_actual_image=" << diagnostic_utf8(image) << '\n'
            << "identity_actual_canonical=" << diagnostic_utf8(canonical) << '\n' << std::flush;
        log << "identity_observation pid_match=" << (GetProcessId(process.value) == session->target_pid)
            << " time_ok=" << time_ok << " time_error=" << time_error
            << " time_match=" << (time_ok && actual_created.dwLowDateTime == session->target_created.dwLowDateTime &&
                actual_created.dwHighDateTime == session->target_created.dwHighDateTime)
            << " image_ok=" << image_ok << " image_error=" << image_error
            << " canonical_nonempty=" << !canonical.empty()
            << " canonical_match=" << (!canonical.empty() && _wcsicmp(canonical.c_str(), session->target_path) == 0)
            << '\n' << std::flush;
    }
    DWORD resume_error = ERROR_SUCCESS;
    bool resumed = false;
    if (bound) {
        SetLastError(ERROR_SUCCESS);
        resumed = ResumeThread(thread.value) != static_cast<DWORD>(-1);
        if (!resumed) resume_error = GetLastError();
        log << "binding_stage=resume ok=" << resumed << " win32=" << resume_error << '\n' << std::flush;
    }
    if (!bound || !resumed) {
        // Abort only our never-resumed child. Preserve the original failure:
        // logging and supplemental queries may overwrite thread LastError.
        log << "pre_resume_abort=1 stage=" << (bound ? "resume" : failed_stage)
            << " error=" << (bound ? resume_error : binding_error) << '\n' << std::flush;
        return 1;
    }
    pending.resumed = true;
    std::cout << "process_started=1 runtime_handshake=0\n";
    std::wcout << L"Owned session: " << directory << std::endl;
    const auto deadline = GetTickCount64() + 10000;
    do {
        cfg::Ready ready;
        if (Ready(directory, *session, ready)) {
            log << "handshake=1 status=" << std::hex << static_cast<unsigned long>(ready.status)
                << " coverage=" << ready.installed_entries << std::dec << '\n' << std::flush;
            return Inspect(directory, false);
        }
        if (WaitForSingleObject(process.value, 100) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(process.value, &code);
            log << "exited_before_handshake=" << code << '\n' << std::flush;
            std::cout << "Not connected: launched process exited, possibly relaying to an existing instance.\n"; return 3;
        }
    } while (GetTickCount64() < deadline);
    log << "handshake=0 application_left_running=1\n" << std::flush;
    std::cout << "No runtime handshake yet; application remains running. Use --status to recheck.\n"; return 3;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--admin-check") {
        const DWORD error = pulse::dialog_admin::VerifyProtectedTree(AdminDrivePath(argv[2]));
        std::cout << "admin_tree_valid=" << !error << " error=" << error << " is_elevated=" << pulse::dialog_admin::IsElevated() << "\n";
        return error ? 2 : 0;
    }
    try {
        if (argc == 3 && (std::wstring_view(argv[1]) == L"--profile" || std::wstring_view(argv[1]) == L"--check-profile")) {
            pulse::dialog_launcher::ManagedLaunchProfile profile;
            auto self = Self(); if (self.starts_with(LR"(\\?\)")) self.erase(0, 4);
            DWORD error = pulse::dialog_launcher::LoadProfile(argv[2], profile);
            if (!error) error = pulse::dialog_launcher::ValidateProfile(profile, self);
            pulse::dialog_bundle::BundleLease bundle;
            if (!error && profile.format_version == 2) error = pulse::dialog_bundle::VerifyBundle(profile.bundle_root, profile.manifest_digest, bundle);
            if (error) { std::cout << "profile_valid=0 error=" << error << " process_started=0 runtime_handshake=0\n"; return 2; }
            if (std::wstring_view(argv[1]) == L"--check-profile") {
                std::cout << "profile_valid=1 process_started=0 runtime_handshake=0\n"; return 0;
            }
            std::vector<std::wstring> arguments{argv[0], L"--host", profile.host, L"--dll", profile.runtime,
                L"--cwd", profile.cwd, L"--control-root", profile.control_root, L"--", profile.target};
            arguments.insert(arguments.end(), profile.arguments.begin(), profile.arguments.end());
            std::vector<wchar_t*> pointers; pointers.reserve(arguments.size());
            for (auto& argument : arguments) pointers.push_back(argument.data());
            return Launch(static_cast<int>(pointers.size()), pointers.data(), bundle ? &bundle : nullptr);
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--disable") return Inspect(argv[2], true);
        if (argc == 3 && std::wstring_view(argv[1]) == L"--status") return Inspect(argv[2], false);
        return Launch(argc, argv);
    } catch (...) { std::cerr << "Launcher failed; no existing application was terminated.\n"; return 1; }
}
