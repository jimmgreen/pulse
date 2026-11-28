#include "user_auto_attach.h"
#include "admin_auto_rebind.h"
#include "window_show_watch.h"
#include "../common/dialog_adaptive_attach.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_auto_binding.h"
#include "../common/dialog_auto_registration.h"
#include "../common/dialog_bundle.h"
#include "../shell_bridge/dialog_application_identity.h"
#include "../shell_bridge/runtime_auto_entry.h"
#include "../shell_bridge/runtime_config.h"
#include <objbase.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace pulse::dialog_launcher::user_auto {
namespace {

namespace fs = std::filesystem;
namespace cfg = pulse::shell_bridge::runtime;
namespace binding = pulse::dialog_auto_binding;
namespace adaptive = pulse::dialog_adaptive;
namespace admin_rebind = pulse::dialog_launcher::admin_auto;

#if defined(_WIN64)
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#endif

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct Module {
    HMODULE value = nullptr;
    ~Module() { if (value) FreeLibrary(value); }
};

DWORD Last() noexcept {
    const DWORD error = GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
}

std::wstring Plain(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return path;
}

bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(Plain(a).c_str(), Plain(b).c_str()) == 0;
}

std::wstring ProcessImage(HANDLE process) {
    std::wstring result(32768, L'\0');
    DWORD units = static_cast<DWORD>(result.size());
    if (!QueryFullProcessImageNameW(process, 0, result.data(), &units)) return {};
    result.resize(units);
    return result;
}

DWORD ProcessMachine(HANDLE process, WORD& value) {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto query = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (!query) return ERROR_NOT_SUPPORTED;
    USHORT process_machine = 0, native = 0;
    if (!query(process, &process_machine, &native)) return Last();
    value = process_machine ? process_machine : native;
    return ERROR_SUCCESS;
}

DWORD WriteNew(const fs::path& path, const void* data, DWORD bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return Last();
    DWORD written = 0;
    if (!WriteFile(file.value, data, bytes, &written, nullptr) || written != bytes || !FlushFileBuffers(file.value)) return Last();
    return ERROR_SUCCESS;
}

DWORD ReadExact(const fs::path& path, void* data, DWORD bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) return Last();
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    DWORD read = 0;
    if (!GetFileInformationByHandle(file.value, &info) || !GetFileSizeEx(file.value, &size)) return Last();
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || size.QuadPart != bytes) return ERROR_INVALID_DATA;
    return ReadFile(file.value, data, bytes, &read, nullptr) && read == bytes ? ERROR_SUCCESS : Last();
}

void DisableSession(const fs::path& directory) {
    if (directory.empty()) return;
    const BYTE value = 1;
    (void)WriteNew(directory / cfg::kDisableFile, &value, 1);
}

DWORD EnsureUserControlRoot(const fs::path& root, const std::wstring& sid_text) {
    const std::wstring sddl = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;" + sid_text + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return Last();
    struct FreeSd { PSECURITY_DESCRIPTOR p; ~FreeSd() { if (p) LocalFree(p); } } guard{sd};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    std::error_code ec;
    fs::create_directories(root.parent_path(), ec);
    if (!CreateDirectoryW(root.c_str(), &sa)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return err ? err : ERROR_GEN_FAILURE;
    }
    const DWORD attrs = GetFileAttributesW(root.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

void CleanupStaleControlDirs(const fs::path& root) {
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const auto mtime = entry.last_write_time(ec);
        if (ec) continue;
        if (now - mtime > std::chrono::minutes(30)) {
            fs::remove_all(entry.path(), ec);
        }
    }
}

struct TargetModule {
    uintptr_t base = 0;
    DWORD size = 0;
    std::wstring path;
};

DWORD EnumTargetModules(DWORD pid, std::vector<TargetModule>& result) {
    result.clear();
    Handle snapshot;
    for (unsigned retry = 0; retry != 12; ++retry) {
        snapshot = Handle(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (snapshot) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return Last();
        Sleep(retry < 4 ? 5 : 20);
    }
    if (!snapshot) return Last();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.value, &entry)) return Last();
    do {
        if (result.size() >= 2048) return ERROR_TOO_MANY_NAMES;
        result.push_back({reinterpret_cast<uintptr_t>(entry.modBaseAddr), entry.modBaseSize, entry.szExePath});
    } while (Module32NextW(snapshot.value, &entry));
    return GetLastError() == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : Last();
}

DWORD RemoteAddress(DWORD pid, const void* local, uintptr_t& address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(local), &module)) return Last();
    Module keep{module};
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768)) return Last();
    const auto offset = reinterpret_cast<uintptr_t>(local) - reinterpret_cast<uintptr_t>(module);
    std::vector<TargetModule> modules;
    if (const DWORD error = EnumTargetModules(pid, modules)) return error;
    for (const auto& target : modules) {
        if (EqualPath(target.path, path)) {
            if (offset >= target.size) return ERROR_BAD_EXE_FORMAT;
            address = target.base + offset;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_MOD_NOT_FOUND;
}

using Key = std::pair<DWORD, uint64_t>;
Key ProcessKey(DWORD pid, FILETIME time) {
    return {pid, (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime};
}

struct Work {
    Handle process, thread;
    DWORD pid = 0;
    FILETIME created{};
    GUID nonce{};
    std::string fingerprint;
    std::wstring basename;
    uint32_t match_mask = 0;
    fs::path control;
    void* remote = nullptr;
    ULONGLONG deadline = 0;
    enum class Phase { Probe, Load, Attach, Ready, Done } phase = Phase::Done;
    bool timeout_logged = false;
    ~Work() {
        if (remote && (!thread || WaitForSingleObject(thread.value, 0) == WAIT_OBJECT_0)) {
            VirtualFreeEx(process.value, remote, 0, MEM_RELEASE);
        }
    }
};

struct WatchedAttached {
    Handle process;
    DWORD pid = 0;
    std::string fingerprint;
    std::wstring basename;
    ULONGLONG attached_tick = 0;
};

DWORD StartRemote(Work& work, uintptr_t address, const void* data, size_t size) {
    if (!address || !size || size > 1024 * 1024) return ERROR_INVALID_PARAMETER;
    work.remote = VirtualAllocEx(work.process.value, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!work.remote) return Last();
    SIZE_T written = 0;
    if (!WriteProcessMemory(work.process.value, work.remote, data, size, &written) || written != size) {
        const DWORD error = Last();
        VirtualFreeEx(work.process.value, work.remote, 0, MEM_RELEASE);
        work.remote = nullptr;
        return error;
    }
    work.thread = Handle(CreateRemoteThread(work.process.value, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), work.remote, 0, nullptr));
    if (!work.thread) {
        const DWORD error = Last();
        VirtualFreeEx(work.process.value, work.remote, 0, MEM_RELEASE);
        work.remote = nullptr;
        return error;
    }
    work.deadline = GetTickCount64() + 10000;
    work.timeout_logged = false;
    return ERROR_SUCCESS;
}

struct Engine {
    const Options& options;
    dialog_bundle::BundleLease bundle;
    Module runtime_module;
    fs::path control_root;
    fs::path store_path;
    adaptive::AdaptiveTargetStore store;
    std::wstring runtime_path;
    std::wstring windows_dir;
    uintptr_t user_attach_offset = 0;
    uintptr_t diagnose_offset = 0;
    uint64_t generation = 0;
    std::vector<std::unique_ptr<Work>> active;
    std::vector<WatchedAttached> watched;
    std::vector<fs::path> sessions;
    std::set<Key> attempted;
    struct PreflightMiss { ULONGLONG retry_at = 0; uint32_t attempts = 0; };
    std::map<Key, PreflightMiss> no_match;
    struct Retry { ULONGLONG next = 0; unsigned count = 0, unavailable = 0; bool authorized = false; };
    std::map<Key, Retry> retries;

    explicit Engine(const Options& input) : options(input) {}

    void Log(const char* stage, DWORD pid = 0, DWORD error = 0, DWORD entries = 0,
             FILETIME created = {}, uint32_t match_mask = 0, const std::wstring& basename = {}) const noexcept {
        try {
            if (options.log) {
                options.log(Event{stage, pid, error, entries, created, match_mask, basename});
            }
        } catch (...) {}
    }

    bool Enabled() const {
        if (!options.require_dialog_auto_enabled) return true;
        return dialog_auto::IsEnabledFor(bundle.Directory() + L"\\" + dialog_auto::DllName);
    }

    bool Stopped() const {
        if (options.stop && WaitForSingleObject(options.stop, 0) != WAIT_TIMEOUT) return true;
        if (options.parent_process && WaitForSingleObject(options.parent_process, 0) != WAIT_TIMEOUT) return true;
        return false;
    }

    void ObserveNativeFileDialog(DWORD pid) {
        if (!pid || pid == GetCurrentProcessId()) return;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!process) return;
        FILETIME created{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process.value, &created, &exit, &kernel, &user)) return;
        WORD machine = 0;
        if (ProcessMachine(process.value, machine) != ERROR_SUCCESS || machine != kMachine) return;
        GUID probe_nonce{};
        if (FAILED(CoCreateGuid(&probe_nonce))) return;
        binding::BindingV1 probe_bound{};
        if (binding::CaptureProcessWithHelperUser(process.value, GetCurrentProcess(), generation, probe_nonce, probe_bound) != ERROR_SUCCESS) return;
        const std::wstring image = ProcessImage(process.value);
        if (image.empty() || adaptive::IsExcludedExecutable(image, windows_dir, options.installed_root)) return;
        const std::string fp = adaptive::ComputeBinaryFingerprintV2(image);
        const std::string legacy_fp = adaptive::ComputeBinaryFingerprint(image);
        if (fp.empty() || store.IsBlocked(fp) || store.IsBlocked(legacy_fp)) return;
        const auto basename = pulse::shell_bridge::application_identity::ExecutableBasename(image);
        (void)store.AdoptLegacyLearned(fp, legacy_fp);
        if (store.RecordLearned(fp, basename)) {
            (void)store.Save(store_path);
            Log("learned_native_dialog", pid, 0, 0, created, adaptive::kMatchLearned, basename);
        }
        const auto key = ProcessKey(pid, created);
        no_match.erase(key);
    }

    DWORD Begin(DWORD pid) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!process) return Last();
        FILETIME created{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process.value, &created, &exit, &kernel, &user)) return Last();
        const auto key = ProcessKey(pid, created);
        if (std::any_of(active.begin(), active.end(), [&](const auto& w) {
            return w->pid == pid && CompareFileTime(&w->created, &created) == 0;
        })) return ERROR_ALREADY_EXISTS;

        if (const auto nm = no_match.find(key); nm != no_match.end()) {
            if (GetTickCount64() < nm->second.retry_at) return ERROR_ALREADY_EXISTS;
        }

        auto retry = retries.find(key);
        if (attempted.contains(key) && (retry == retries.end() || GetTickCount64() < retry->second.next)) {
            return ERROR_ALREADY_EXISTS;
        }
        if (retry != retries.end() && retry->second.count >= admin_rebind::kMaxAutoRebinds) {
            retry->second.next = MAXULONGLONG;
            return ERROR_ALREADY_EXISTS;
        }
        if (retry != retries.end()) retry->second.next = GetTickCount64() + admin_rebind::kAutoRebindCooldownMs;

        GUID nonce{};
        if (FAILED(CoCreateGuid(&nonce))) return ERROR_GEN_FAILURE;
        binding::BindingV1 bound{};
        DWORD error = binding::CaptureProcessWithHelperUser(process.value, GetCurrentProcess(), generation, nonce, bound);
        if (error) return error;

        WORD machine = 0;
        if ((error = ProcessMachine(process.value, machine)) || machine != kMachine) {
            return error ? error : ERROR_BAD_EXE_FORMAT;
        }
        // Resolved at run time: pulse.exe links this code and must not statically import a
        // Windows 8+ API (Windows 8.1 startup guard).
        static const auto query_protection = reinterpret_cast<decltype(&GetProcessInformation)>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessInformation"));
        PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
        if (!query_protection ||
            !query_protection(process.value, ProcessProtectionLevelInfo, &protection, sizeof(protection)) ||
            protection.ProtectionLevel != PROTECTION_LEVEL_NONE) {
            return ERROR_ACCESS_DENIED;
        }
        const std::wstring image = ProcessImage(process.value);
        if (image.empty()) return Last();
        if (pid == GetCurrentProcessId() || adaptive::IsExcludedExecutable(image, windows_dir, options.installed_root)) {
            if (attempted.size() < 8192) attempted.insert(key);
            return ERROR_NOT_SUPPORTED;
        }

        // Version-stable key; the legacy size/mtime key is still honoured for
        // existing learned and blocked entries and migrated on first use.
        const std::string fp = adaptive::ComputeBinaryFingerprintV2(image);
        const std::string legacy_fp = adaptive::ComputeBinaryFingerprint(image);
        const std::wstring basename = pulse::shell_bridge::application_identity::ExecutableBasename(image);
        if (store.IsBlocked(fp) || store.IsBlocked(legacy_fp)) {
            if (attempted.size() < 8192) attempted.insert(key);
            Log("circuit_breaker_blocked", pid, ERROR_ACCESS_DISABLED_BY_POLICY, 0, created, 0, basename);
            return ERROR_ACCESS_DISABLED_BY_POLICY;
        }

        Handle privileged(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
            PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, pid));
        if (!privileged) return Last();
        FILETIME reopened{};
        if (!GetProcessTimes(privileged.value, &reopened, &exit, &kernel, &user) || CompareFileTime(&reopened, &created) != 0) {
            return ERROR_INVALID_DATA;
        }
        process = std::move(privileged);

        if (store.AdoptLegacyLearned(fp, legacy_fp)) (void)store.Save(store_path);
        uint32_t match_mask = store.IsLearned(fp) ? adaptive::kMatchLearned : adaptive::kMatchNone;
        if (!match_mask) {
            uint32_t scanned_mask = 0;
            const DWORD scan_err = adaptive::ScanRemoteProcessForAdaptiveMatch(
                process.value, pid, kMachine, windows_dir, options.installed_root, scanned_mask);
            if (scan_err == ERROR_SUCCESS) match_mask |= scanned_mask;
        }
        if (!match_mask) {
            auto& miss = no_match[key];
            ++miss.attempts;
            const ULONGLONG delay_ms = miss.attempts == 1 ? 5000ull :
                miss.attempts == 2 ? 20000ull :
                miss.attempts == 3 ? 60000ull : 120000ull;
            miss.retry_at = GetTickCount64() + delay_ms;
            return ERROR_NOT_FOUND;
        }
        no_match.erase(key);

        if (!attempted.contains(key) && attempted.size() >= 8192) return ERROR_TOO_MANY_NAMES;
        attempted.insert(key);
        Log("adaptive_matched", pid, 0, 0, created, match_mask, basename);

        std::vector<TargetModule> modules;
        if ((error = EnumTargetModules(pid, modules))) return error;
        uintptr_t existing = 0;
        for (const auto& item : modules) {
            const auto name = fs::path(item.path).filename().wstring();
            if (name.starts_with(L"PulseFileDialogRuntime") || name.starts_with(L"PulseFileDialogAutoRuntime")) {
                if (!admin_rebind::CanReuseAutoRuntime(EqualPath(item.path, runtime_path), existing != 0,
                                                       user_attach_offset, diagnose_offset, item.size)) {
                    retries.erase(key);
                    return ERROR_ALREADY_INITIALIZED;
                }
                existing = item.base;
            }
        }
        if (retry != retries.end() && !existing) {
            retries.erase(key);
            return ERROR_MOD_NOT_FOUND;
        }
        if ((error = dialog_bundle::RecheckDirectory(bundle))) return error;
        if (!Enabled() || Stopped()) return ERROR_CANCELLED;

        auto work = std::make_unique<Work>();
        work->process = std::move(process);
        work->pid = pid;
        work->created = created;
        work->nonce = nonce;
        work->fingerprint = fp;
        work->basename = basename;
        work->match_mask = match_mask;

        if (existing && (retry == retries.end() || !retry->second.authorized)) {
            cfg::PulseRuntimeAutoDiagnosticV1 diagnostic{};
            auto& state = retries[key];
            state.next = GetTickCount64() + admin_rebind::kAutoRebindCooldownMs;
            if ((error = StartRemote(*work, existing + diagnose_offset, &diagnostic, sizeof(diagnostic)))) {
                retries.erase(key);
                return error;
            }
            work->phase = Work::Phase::Probe;
            active.push_back(std::move(work));
            return ERROR_SUCCESS;
        }
        if (existing) {
            retry->second.authorized = false;
            ++retry->second.count;
        }
        if (sessions.size() >= 8192) {
            retries.erase(key);
            return ERROR_TOO_MANY_NAMES;
        }

        wchar_t nonce_text[40]{};
        StringFromGUID2(nonce, nonce_text, 40);
        work->control = control_root / nonce_text;
        if (!CreateDirectoryW(work->control.c_str(), nullptr)) return Last();

        auto session = std::make_unique<cfg::Session>();
        session->reserved = cfg::kUserAutoSession;
        session->target_pid = pid;
        session->target_created = created;
        session->machine = kMachine;
        wcscpy_s(session->nonce, nonce_text);
        wcscpy_s(session->host_path, bundle.Host().c_str());
        wcscpy_s(session->target_path, image.c_str());
        wcscpy_s(session->control_path, work->control.c_str());
        wcscpy_s(session->launcher_path, bundle.Launcher().c_str());

        if ((error = WriteNew(work->control / cfg::kSessionFile, session.get(), sizeof(*session))) ||
            (error = WriteNew(work->control / L"auto-binding.bin", &bound, sizeof(bound)))) {
            DisableSession(work->control);
            return error;
        }
        sessions.push_back(work->control);
        if (!Enabled() || Stopped()) {
            DisableSession(work->control);
            return ERROR_CANCELLED;
        }

        if (existing) {
            auto request = std::make_unique<cfg::PulseRuntimeAutoStartV1>();
            StringFromGUID2(work->nonce, request->nonce, 40);
            wcscpy_s(request->host, bundle.Host().c_str());
            wcscpy_s(request->control, work->control.c_str());
            if ((error = StartRemote(*work, existing + user_attach_offset, request.get(), sizeof(*request)))) {
                DisableSession(work->control);
                return error;
            }
            work->phase = Work::Phase::Attach;
            active.push_back(std::move(work));
            Log("user_rebind_queued", pid, 0, 0, created, match_mask, basename);
            return ERROR_SUCCESS;
        }

        uintptr_t load = 0;
        if ((error = RemoteAddress(pid, reinterpret_cast<const void*>(&LoadLibraryW), load))) {
            DisableSession(work->control);
            return error;
        }
        if ((error = StartRemote(*work, load, runtime_path.c_str(), (runtime_path.size() + 1) * sizeof(wchar_t)))) {
            DisableSession(work->control);
            return error;
        }
        retries[key].next = GetTickCount64() + admin_rebind::kAutoRebindCooldownMs;
        work->phase = Work::Phase::Load;
        active.push_back(std::move(work));
        Log("user_load_queued", pid, 0, 0, created, match_mask, basename);
        return ERROR_SUCCESS;
    }

    void TripFailure(const Work& work, bool immediate_block, const char* stage, DWORD error) {
        const bool blocked = store.RecordFailure(work.fingerprint, work.basename, immediate_block);
        (void)store.Save(store_path);
        Log(blocked ? "circuit_breaker_tripped" : stage, work.pid, error, 0, work.created, work.match_mask, work.basename);
    }

    void Tick() {
        const ULONGLONG now = GetTickCount64();
        for (auto it = watched.begin(); it != watched.end();) {
            const DWORD wait = WaitForSingleObject(it->process.value, 0);
            if (wait == WAIT_OBJECT_0) {
                DWORD code = 0;
                if (GetExitCodeProcess(it->process.value, &code) && adaptive::IsCrashExitCode(code)) {
                    store.RecordFailure(it->fingerprint, it->basename, true);
                    (void)store.Save(store_path);
                    Log("circuit_breaker_crash_tripped", it->pid, code, 0, {}, 0, it->basename);
                }
                it = watched.erase(it);
            } else if (wait != WAIT_TIMEOUT || now - it->attached_tick >= 5000) {
                it = watched.erase(it);
            } else {
                ++it;
            }
        }

        for (auto& pointer : active) {
            auto& work = *pointer;
            if (work.phase == Work::Phase::Done) continue;
            if (WaitForSingleObject(work.process.value, 0) != WAIT_TIMEOUT) {
                DWORD code = 0;
                if (GetExitCodeProcess(work.process.value, &code) && adaptive::IsCrashExitCode(code)) {
                    TripFailure(work, true, "target_crashed", code);
                } else {
                    Log("target_exited", work.pid, 0, 0, work.created, work.match_mask, work.basename);
                }
                DisableSession(work.control);
                work.phase = Work::Phase::Done;
                continue;
            }
            if (work.phase == Work::Phase::Ready) {
                cfg::Ready ready{};
                const DWORD error = ReadExact(work.control / cfg::kReadyFile, &ready, sizeof(ready));
                if (!error) {
                    wchar_t nonce[40]{};
                    StringFromGUID2(work.nonce, nonce, 40);
                    const bool identity = ready.magic == cfg::kReadyMagic && ready.version == cfg::kVersion &&
                        ready.size == sizeof(ready) && !ready.reserved && ready.target_pid == work.pid &&
                        ready.machine == kMachine && CompareFileTime(&ready.target_created, &work.created) == 0 &&
                        wcsncmp(ready.nonce, nonce, 40) == 0;
                    const bool accepted = identity && ready.status == S_OK && ready.installed_entries;
                    if (accepted) {
                        store.ClearFailureCount(work.fingerprint);
                        retries.erase(ProcessKey(work.pid, work.created));
                        Log("user_ready", work.pid, 0, ready.installed_entries, work.created, work.match_mask, work.basename);
                        watched.push_back(WatchedAttached{std::move(work.process), work.pid, work.fingerprint, work.basename, GetTickCount64()});
                    } else {
                        DisableSession(work.control);
                        TripFailure(work, false, "ready_rejected", ERROR_INVALID_DATA);
                    }
                    work.phase = Work::Phase::Done;
                } else if (error != ERROR_FILE_NOT_FOUND || GetTickCount64() > work.deadline) {
                    DisableSession(work.control);
                    TripFailure(work, error == ERROR_FILE_NOT_FOUND, "ready_failed", error == ERROR_FILE_NOT_FOUND ? ERROR_TIMEOUT : error);
                    work.phase = Work::Phase::Done;
                }
                continue;
            }

            const DWORD waited = WaitForSingleObject(work.thread.value, 0);
            if (waited == WAIT_TIMEOUT) {
                if (GetTickCount64() > work.deadline && !work.timeout_logged) {
                    work.timeout_logged = true;
                    DisableSession(work.control);
                    TripFailure(work, true, "remote_thread_timeout", ERROR_TIMEOUT);
                }
                continue;
            }
            if (waited != WAIT_OBJECT_0) {
                DisableSession(work.control);
                TripFailure(work, false, "remote_thread_wait_failed", Last());
                work.phase = Work::Phase::Done;
                continue;
            }
            DWORD result = 0;
            GetExitCodeThread(work.thread.value, &result);
            if (work.phase == Work::Phase::Probe) {
                cfg::PulseRuntimeAutoDiagnosticV1 diagnostic{};
                SIZE_T read = 0;
                const bool valid = !result && !work.timeout_logged &&
                    ReadProcessMemory(work.process.value, work.remote, &diagnostic, sizeof(diagnostic), &read) &&
                    read == sizeof(diagnostic);
                const auto key = ProcessKey(work.pid, work.created);
                if (valid && admin_rebind::CanAutoRebind(diagnostic)) {
                    auto& state = retries[key];
                    state.authorized = true;
                    state.unavailable = 0;
                    state.next = 0;
                } else if (!valid) {
                    retries.erase(key);
                } else if (!admin_rebind::ShouldContinueProbing(diagnostic)) {
                    retries.erase(key);
                } else if (diagnostic.initialization != S_OK && diagnostic.initialization != E_PENDING) {
                    if (++retries[key].unavailable >= 3) retries.erase(key);
                } else {
                    retries[key].unavailable = 0;
                }
                work.phase = Work::Phase::Done;
            }
            work.thread = Handle();
            if (work.remote) {
                VirtualFreeEx(work.process.value, work.remote, 0, MEM_RELEASE);
                work.remote = nullptr;
            }
            if (work.timeout_logged || work.phase == Work::Phase::Done) {
                work.phase = Work::Phase::Done;
                continue;
            }
            if (work.phase == Work::Phase::Attach) {
                if (static_cast<HRESULT>(result) != S_OK) {
                    DisableSession(work.control);
                    TripFailure(work, false, "attach_rejected", result);
                    work.phase = Work::Phase::Done;
                } else {
                    work.phase = Work::Phase::Ready;
                    work.deadline = GetTickCount64() + 15000;
                }
                continue;
            }

            std::vector<TargetModule> modules;
            DWORD error = EnumTargetModules(work.pid, modules);
            uintptr_t address = 0;
            if (!error) {
                for (const auto& loaded : modules) {
                    if (EqualPath(loaded.path, runtime_path) && user_attach_offset < loaded.size) {
                        address = loaded.base + user_attach_offset;
                        break;
                    }
                }
            }
            if (!address) {
                DisableSession(work.control);
                TripFailure(work, false, "loaded_module_missing", error ? error : ERROR_MOD_NOT_FOUND);
                work.phase = Work::Phase::Done;
                continue;
            }
            auto request = std::make_unique<cfg::PulseRuntimeAutoStartV1>();
            StringFromGUID2(work.nonce, request->nonce, 40);
            wcscpy_s(request->host, bundle.Host().c_str());
            wcscpy_s(request->control, work.control.c_str());
            if (!Enabled() || Stopped()) {
                DisableSession(work.control);
                work.phase = Work::Phase::Done;
                continue;
            }
            error = StartRemote(work, address, request.get(), sizeof(*request));
            if (error) {
                DisableSession(work.control);
                TripFailure(work, false, "attach_thread_failed", error);
                work.phase = Work::Phase::Done;
            } else {
                work.phase = Work::Phase::Attach;
            }
        }
        std::erase_if(active, [](const auto& work) { return work->phase == Work::Phase::Done; });
    }

    ~Engine() {
        for (const auto& session : sessions) DisableSession(session);
    }
};

} // namespace

DWORD Run(const Options& options) noexcept {
    try {
        if (options.installed_root.empty() || options.data_dir.empty()) return ERROR_INVALID_PARAMETER;
        if (dialog_admin::IsElevated()) return ERROR_ACCESS_DENIED;

        Engine engine(options);
        DWORD error = dialog_bundle::ResolveInstalledBundleForMachine(options.installed_root, kMachine, engine.bundle);
        if (error) {
            engine.Log("user_bundle_rejected", 0, error);
            return error;
        }
        wchar_t windows[32768]{};
        if (!GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) return Last();
        engine.windows_dir = windows;

        GUID self_nonce{};
        if (FAILED(CoCreateGuid(&self_nonce))) return ERROR_GEN_FAILURE;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        engine.generation = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;

        binding::BindingV1 self{};
        if ((error = binding::CaptureCurrentWithHelperUser(GetCurrentProcess(), engine.generation, self_nonce, self))) {
            return error;
        }
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(self.target.user_sid, &sid)) return Last();
        const std::wstring sid_text = sid;
        LocalFree(sid);

        Handle root_handle(CreateFileW(options.installed_root.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        BY_HANDLE_FILE_INFORMATION root_info{};
        if (!root_handle || !GetFileInformationByHandle(root_handle.value, &root_info)) return Last();
        const std::wstring discriminator = std::to_wstring(root_info.dwVolumeSerialNumber) + L"-" +
            std::to_wstring(root_info.nFileIndexHigh) + L"-" + std::to_wstring(root_info.nFileIndexLow);
        const std::wstring mutex_name = L"Local\\Pulse.UserDialogAuto." + sid_text + L"." +
            std::to_wstring(self.target.session_id) + L"." + std::to_wstring(kMachine) + L"." + discriminator;
        Handle mutex(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
        if (!mutex) return Last();
        if (GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_ALREADY_INITIALIZED;

        engine.control_root = fs::path(options.data_dir) / L"DialogUserAuto";
        if ((error = EnsureUserControlRoot(engine.control_root, sid_text))) return error;
        CleanupStaleControlDirs(engine.control_root);

        engine.store_path = fs::path(options.data_dir) / L"dialog_adaptive_targets.json";
        (void)engine.store.Load(engine.store_path);

        if (!engine.Enabled()) {
            engine.Log("user_disabled_exit");
            return ERROR_CANCELLED;
        }

        engine.runtime_path = engine.bundle.AutoRuntime();
        engine.runtime_module.value = LoadLibraryExW(engine.runtime_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!engine.runtime_module.value) return Last();

        auto attach_fn = GetProcAddress(engine.runtime_module.value, "PulseFileDialogRuntimeUserAutoAttach");
        if (!attach_fn) return ERROR_PROC_NOT_FOUND;
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(attach_fn), &owner)) return Last();
        Module export_owner{owner};
        if (owner != engine.runtime_module.value) return ERROR_INVALID_DLL;
        engine.user_attach_offset = reinterpret_cast<uintptr_t>(attach_fn) - reinterpret_cast<uintptr_t>(engine.runtime_module.value);

        if (auto diag_fn = GetProcAddress(engine.runtime_module.value, "PulseFileDialogRuntimeAutoDiagnose")) {
            HMODULE diag_owner = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(diag_fn), &diag_owner)) {
                Module diag_keep{diag_owner};
                if (diag_owner == engine.runtime_module.value) {
                    engine.diagnose_offset = reinterpret_cast<uintptr_t>(diag_fn) - reinterpret_cast<uintptr_t>(engine.runtime_module.value);
                }
            }
        }

        Handle companion_process, companion_job;
#if defined(_WIN64)
        unsigned companion_restarts = 0;
        ULONGLONG companion_retry_at = 0;
        const auto start_companion = [&]() {
            if (options.companion) return;
            dialog_bundle::BundleLease companion;
            const DWORD resolve_err = dialog_bundle::ResolveInstalledBundleForMachine(
                options.installed_root, IMAGE_FILE_MACHINE_I386, companion);
            if (resolve_err != ERROR_SUCCESS || companion.AdminAutoHelper().empty()) {
                engine.Log("user_x86_companion_not_present", 0, resolve_err);
                companion_restarts = 5; // Bundle absent; do not spam retries.
                return;
            }
            if (!companion_job) {
                companion_job = Handle(CreateJobObjectW(nullptr, nullptr));
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!companion_job || !SetInformationJobObject(companion_job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                    engine.Log("user_x86_companion_failed", 0, Last());
                    companion_job = Handle();
                    return;
                }
            }
            std::wstring cmd = L"\"" + companion.AdminAutoHelper() + L"\" --user-adaptive --companion --installed-root \"" +
                options.installed_root + L"\" --parent-pid " + std::to_wstring(GetCurrentProcessId());
            if (!options.require_dialog_auto_enabled) cmd += L" --no-reg-check";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(companion.AdminAutoHelper().c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, options.installed_root.c_str(), &startup, &child)) {
                engine.Log("user_x86_companion_failed", 0, Last());
                return;
            }
            companion_process = Handle(child.hProcess);
            Handle thread(child.hThread);
            if (!AssignProcessToJobObject(companion_job.value, child.hProcess) ||
                ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
                const DWORD err = Last();
                TerminateProcess(child.hProcess, ERROR_GEN_FAILURE);
                companion_process = Handle();
                engine.Log("user_x86_companion_failed", child.dwProcessId, err);
                return;
            }
            engine.Log("user_x86_companion_started", child.dwProcessId);
        };
        start_companion();
#endif

        engine.Log("user_helper_ready", GetCurrentProcessId(), 0, kMachine, self.target.created);

        ULONGLONG next_scan = 0;
        const ULONGLONG began = GetTickCount64();
        // New windows are checked as they appear; the sweep below stays as the fallback.
        WindowShowWatch watch;
        if (watch.Start(&adaptive::IsNativeFileDialogWindow)) engine.Log("window_watch_ready");
        else engine.Log("window_watch_unavailable", 0, GetLastError());
        while (engine.Enabled() && !engine.Stopped()) {
            if (options.duration_ms && GetTickCount64() - began >= options.duration_ms) break;
            // The settings page may resume a paused program by rewriting the store.
            if (engine.store.ReloadIfChanged(engine.store_path)) engine.Log("adaptive_store_reloaded");
#if defined(_WIN64)
            if (!options.companion) {
                if (companion_process && WaitForSingleObject(companion_process.value, 0) == WAIT_OBJECT_0) {
                    DWORD exit_code = 0;
                    GetExitCodeProcess(companion_process.value, &exit_code);
                    engine.Log("user_x86_companion_exited", 0, exit_code);
                    companion_process = Handle();
                    if (exit_code != ERROR_ALREADY_INITIALIZED && exit_code != ERROR_CANCELLED && companion_restarts < 5) {
                        const ULONGLONG delay_ms = companion_restarts == 0 ? 5000ull :
                            companion_restarts == 1 ? 15000ull : 30000ull;
                        companion_retry_at = GetTickCount64() + delay_ms;
                        ++companion_restarts;
                    }
                } else if (!companion_process && companion_retry_at && GetTickCount64() >= companion_retry_at) {
                    companion_retry_at = 0;
                    start_companion();
                }
            }
#endif
            engine.Tick();
            for (const auto& shown : watch.Take()) {
                if (shown.classified) engine.ObserveNativeFileDialog(shown.pid);
                if (engine.active.size() >= 4 || engine.Stopped() || !engine.Enabled()) continue;
                (void)engine.Begin(shown.pid);
            }
            if (GetTickCount64() >= next_scan && engine.active.size() < 4) {
                next_scan = GetTickCount64() + 500;
                struct WindowScan {
                    std::set<DWORD> pids;
                    std::set<DWORD> native_dialog_pids;
                } scan;
                EnumWindows([](HWND hwnd, LPARAM raw) -> BOOL {
                    if (!IsWindowVisible(hwnd)) return TRUE;
                    auto& out = *reinterpret_cast<WindowScan*>(raw);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (!pid) return TRUE;
                    out.pids.insert(pid);
                    if (adaptive::IsNativeFileDialogWindow(hwnd)) {
                        out.native_dialog_pids.insert(pid);
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&scan));

                for (DWORD pid : scan.native_dialog_pids) {
                    engine.ObserveNativeFileDialog(pid);
                }
                for (DWORD pid : scan.pids) {
                    if (pid == GetCurrentProcessId()) continue;
                    if (engine.active.size() >= 4 || engine.Stopped() || !engine.Enabled()) break;
                    (void)engine.Begin(pid);
                }
            }
            HANDLE waits[2]{};
            DWORD wait_count = 0;
            if (options.stop) waits[wait_count++] = options.stop;
            if (options.parent_process) waits[wait_count++] = options.parent_process;
            watch.Wait(waits, wait_count, 100);
        }

        engine.Log("user_helper_stopping", GetCurrentProcessId());
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

} // namespace pulse::dialog_launcher::user_auto
