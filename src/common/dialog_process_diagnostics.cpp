#include "dialog_process_diagnostics.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cwchar>
#include <set>
#include <vector>

namespace pulse::dialog_diagnostics {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::optional<uint64_t> Creation(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
    return (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
}
bool Alive(HANDLE process) {
    DWORD exit = 0;
    return GetExitCodeProcess(process, &exit) && exit == STILL_ACTIVE;
}
unsigned Architecture(HANDLE process) {
    using Query = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (query) {
        USHORT machine = 0, native = 0;
        if (!query(process, &machine, &native)) return 0;
        if (machine == IMAGE_FILE_MACHINE_I386 || (!machine && native == IMAGE_FILE_MACHINE_I386)) return 32;
        if (machine == IMAGE_FILE_MACHINE_AMD64 || (!machine && native == IMAGE_FILE_MACHINE_AMD64)) return 64;
        return 0;
    }
    BOOL wow = FALSE; SYSTEM_INFO info{}; GetNativeSystemInfo(&info);
    if (!IsWow64Process(process, &wow)) return 0;
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return 32;
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? (wow ? 32u : 64u) : 0u;
}
void Component(ProcessSnapshot& result, const wchar_t* name) {
    if (!_wcsicmp(name, L"PulseDialogAuto64.dll") || !_wcsicmp(name, L"PulseDialogAuto32.dll")) result.loaded_components[0] = true;
    if (!_wcsicmp(name, L"PulseFileDialogRuntime64.dll") || !_wcsicmp(name, L"PulseFileDialogRuntime32.dll") ||
        !_wcsicmp(name, L"PulseFileDialogRuntimeUiProof64R8.dll") || !_wcsicmp(name, L"PulseFileDialogRuntimeUiProof32R8.dll")) result.loaded_components[1] = true;
    if (!_wcsicmp(name, L"PulsePickerUiProof.dll")) result.loaded_components[2] = true;
}
struct Windows {
    std::set<DWORD> pids;
    bool limited = false;
};
BOOL CALLBACK Visit(HWND window, LPARAM parameter) {
    auto& state = *reinterpret_cast<Windows*>(parameter);
    if (!IsWindowVisible(window)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (!pid || state.pids.contains(pid)) return TRUE;
    if (state.pids.size() == 64) { state.limited = true; return FALSE; }
    state.pids.insert(pid);
    return TRUE;
}
std::string Number(const std::optional<uint64_t>& value) { return value ? std::to_string(*value) : "null"; }
}

static ProcessSnapshot CaptureProcessImpl(DWORD pid, RestartProcessSnapshot* local) {
    ProcessSnapshot result; result.pid = pid; result.sample_tick_ms = GetTickCount64();
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value) { result.process_query_error = GetLastError(); return result; }
    if (local) {
        DWORD own_session = 0, target_session = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || !ProcessIdToSessionId(pid, &target_session)) {
            local->scope_error = GetLastError(); return result;
        }
        if (own_session != target_session) return result;
        Handle own_token, target_token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_token.value) ||
            !OpenProcessToken(process.value, TOKEN_QUERY, &target_token.value)) {
            local->scope_error = GetLastError(); return result;
        }
        std::vector<BYTE> own(1024), target(1024); DWORD size = 0;
        if (!GetTokenInformation(own_token.value, TokenUser, own.data(), static_cast<DWORD>(own.size()), &size) ||
            !GetTokenInformation(target_token.value, TokenUser, target.data(), static_cast<DWORD>(target.size()), &size)) {
            local->scope_error = GetLastError(); return result;
        }
        if (!EqualSid(reinterpret_cast<TOKEN_USER*>(own.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(target.data())->User.Sid)) return result;
        local->same_user_session = true;
    }
    result.creation_time = Creation(process.value);
    if (!result.creation_time) result.process_query_error = GetLastError();
    result.process_arch = Architecture(process.value);
    wchar_t image[32768]{}; DWORD units = ARRAYSIZE(image);
    if (QueryFullProcessImageNameW(process.value, 0, image, &units)) {
        if (local) local->image_path.assign(image, units);
        const auto slash = wcsrchr(image, L'\\'); const auto base = slash ? slash + 1 : image;
        uint64_t id = 14695981039346656037ull;
        for (const auto* p = base; *p; ++p) { id ^= static_cast<uint64_t>(*p >= L'A' && *p <= L'Z' ? *p + (L'a' - L'A') : *p); id *= 1099511628211ull; }
        result.app_image_id = id;
    } else if (local) result.process_query_error = GetLastError();
    HANDLE raw = nullptr;
    if (OpenProcessToken(process.value, TOKEN_QUERY, &raw)) {
        Handle token{raw}; TOKEN_ELEVATION value{}; DWORD size = 0;
        if (GetTokenInformation(token.value, TokenElevation, &value, sizeof(value), &size)) result.elevated = value.TokenIsElevated != 0;
        else result.elevation_error = GetLastError();
    } else result.elevation_error = GetLastError();
    Handle modules;
    for (unsigned retry = 0; retry < 3; ++retry) {
        modules.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (modules.value != INVALID_HANDLE_VALUE) break;
        result.module_error = GetLastError();
        if (result.module_error != ERROR_BAD_LENGTH) break;
    }
    if (modules.value == INVALID_HANDLE_VALUE) {
        result.module_status = result.module_error == ERROR_ACCESS_DENIED ? ModuleStatus::AccessDenied : ModuleStatus::Unavailable;
    } else {
        result.module_error = 0;
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        BOOL found = Module32FirstW(modules.value, &entry); unsigned count = 0;
        if (!found) result.module_error = GetLastError();
        while (found) {
            Component(result, entry.szModule);
            if (local) {
                ProcessSnapshot component; Component(component, entry.szModule);
                if (std::any_of(component.loaded_components.begin(), component.loaded_components.end(), [](bool loaded) { return loaded; })) {
                    if (wcslen(entry.szExePath) >= MAX_PATH - 1) { result.module_error = ERROR_MORE_DATA; break; }
                    local->module_paths.emplace_back(entry.szExePath);
                }
            }
            if (++count >= 4096) { result.module_error = ERROR_MORE_DATA; break; }
            entry.dwSize = sizeof(entry); found = Module32NextW(modules.value, &entry);
            if (!found) result.module_error = GetLastError();
        }
        if (result.module_error == ERROR_NO_MORE_FILES) { result.module_error = 0; result.module_status = ModuleStatus::Complete; }
        else result.module_status = ModuleStatus::Partial;
    }
    Handle after{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    result.identity_verified = result.creation_time && after.value && Alive(process.value) && Alive(after.value) && Creation(after.value) == result.creation_time;
    if (!result.identity_verified) {
        result.module_status = ModuleStatus::Unavailable; result.loaded_components = {};
        if (local) local->module_paths.clear();
        if (!result.module_error) result.module_error = ERROR_RETRY;
    }
    return result;
}

ProcessSnapshot CaptureProcess(DWORD pid) { return CaptureProcessImpl(pid, nullptr); }
RestartProcessSnapshot CaptureRestartProcess(DWORD pid) {
    RestartProcessSnapshot result; result.process = CaptureProcessImpl(pid, &result); return result;
}
std::string Serialize(const ProcessSnapshot& snapshot) {
    auto state = snapshot.module_status;
    if (!snapshot.identity_verified || !snapshot.creation_time || snapshot.process_query_error) state = ModuleStatus::Unavailable;
    if (state == ModuleStatus::Complete && snapshot.module_error) state = ModuleStatus::Partial;
    const char* status = state == ModuleStatus::Complete ? "complete" : state == ModuleStatus::Partial ? "partial" : state == ModuleStatus::AccessDenied ? "access_denied" : "unavailable";
    std::string components;
    constexpr const char* names[]{"auto", "runtime", "picker_ui"};
    if (state == ModuleStatus::Complete || state == ModuleStatus::Partial) {
        for (size_t i = 0; i < snapshot.loaded_components.size(); ++i) if (snapshot.loaded_components[i]) {
            if (!components.empty()) components += ',';
            components += '"'; components += names[i]; components += '"';
        }
    }
    return "{\"schema\":1,\"event\":\"process_snapshot\",\"pid\":" + std::to_string(snapshot.pid) + ",\"data\":{\"snapshot_id\":\"" +
        std::to_string(snapshot.pid) + ":" + Number(snapshot.creation_time) + "\",\"scope\":\"current_collector_context\",\"evidence_scope\":\"observed_process_metadata_only\"," +
        "\"process_arch\":" + (snapshot.process_arch ? std::to_string(snapshot.process_arch) : "\"unknown\"") + ",\"process_query_error\":" + std::to_string(snapshot.process_query_error) +
        ",\"creation_time_filetime\":" + Number(snapshot.creation_time) + ",\"app_image_id\":" + Number(snapshot.app_image_id) + ",\"sample_tick_ms\":" + std::to_string(snapshot.sample_tick_ms) +
        ",\"elevated\":" + (snapshot.elevated ? (*snapshot.elevated ? "true" : "false") : "null") + ",\"elevation_error\":" + std::to_string(snapshot.elevation_error) +
        ",\"module_status\":\"" + status + "\",\"module_error\":" + std::to_string(snapshot.module_error) + ",\"loaded_components\":[" + components + "]}}\n";
}

std::string CaptureVisibleProcesses(const std::function<bool()>& cancelled) {
    const auto begin = GetTickCount64(); Windows windows;
    SetLastError(ERROR_SUCCESS); const BOOL complete = EnumWindows(Visit, reinterpret_cast<LPARAM>(&windows));
    const auto error = complete || windows.limited ? ERROR_SUCCESS : GetLastError();
    std::string result; unsigned count = 0; bool stopped = false, budget = false;
    for (const auto pid : windows.pids) {
        if (cancelled && cancelled()) { stopped = true; break; }
        if (GetTickCount64() - begin >= 2000) { budget = true; break; }
        result += Serialize(CaptureProcess(pid)); ++count;
    }
    result += "{\"schema\":1,\"event\":\"process_snapshot_collection\",\"data\":{\"scope\":\"visible_top_level_window_processes\",\"counts_as_requests\":false,\"snapshot_count\":" + std::to_string(count) +
        ",\"enumeration_error\":" + std::to_string(error) + ",\"limit_reached\":" + (windows.limited ? "true" : "false") + ",\"cancelled\":" + (stopped ? "true" : "false") + ",\"budget_exhausted\":" + (budget ? "true" : "false") + "}}\n";
    return result;
}
}
