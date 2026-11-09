#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "isolation.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <userenv.h>
#include <vector>

namespace pulse::extension_host {
namespace {
bool TraceLoader(HANDLE process, DWORD process_id) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    for (unsigned count = 0; count < 4096 && GetTickCount64() < deadline; ++count) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEventEx(&event, 250)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT)
                continue;
            break;
        }
        DWORD continuation = DBG_CONTINUE;
        HANDLE image = nullptr;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
            image = event.u.CreateProcessInfo.hFile;
        else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)
            image = event.u.LoadDll.hFile;
        if (image) {
            wchar_t path[32768]{};
            const DWORD length = GetFinalPathNameByHandleW(image, path, 32768, FILE_NAME_NORMALIZED);
            std::wprintf(L"[LOADER] image=%ls\n", length && length < 32768 ? path : L"<unresolved>");
            CloseHandle(image);
        }
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto &exception = event.u.Exception;
            std::wprintf(L"[LOADER] exception=0x%08lx firstChance=%lu address=%p\n",
                         exception.ExceptionRecord.ExceptionCode, exception.dwFirstChance,
                         exception.ExceptionRecord.ExceptionAddress);
            for (DWORD i = 0; i < exception.ExceptionRecord.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
                std::wprintf(L"[LOADER] parameter[%lu]=0x%llx\n", i,
                             static_cast<unsigned long long>(exception.ExceptionRecord.ExceptionInformation[i]));
            if (exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT)
                continuation = DBG_EXCEPTION_NOT_HANDLED;
        } else if (event.dwDebugEventCode == OUTPUT_DEBUG_STRING_EVENT) {
            const auto &info = event.u.DebugString;
            const SIZE_T units = info.nDebugStringLength < 4096 ? info.nDebugStringLength : 4096;
            SIZE_T read = 0;
            if (info.fUnicode) {
                wchar_t message[4097]{};
                if (ReadProcessMemory(process, info.lpDebugStringData, message, units * sizeof(wchar_t), &read))
                    std::wprintf(L"[LOADER] text=%ls\n", message);
            } else {
                char message[4097]{};
                if (ReadProcessMemory(process, info.lpDebugStringData, message, units, &read))
                    std::wprintf(L"[LOADER] text=%hs\n", message);
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            std::wprintf(L"[LOADER] exit=0x%08lx\n", event.u.ExitProcess.dwExitCode);
        const bool exited = event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT;
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation))
            break;
        if (exited)
            return true;
    }
    // A diagnostic timeout must not leave a debuggee stopped outside containment.
    TerminateProcess(process, ERROR_TIMEOUT);
    DebugActiveProcessStop(process_id);
    return false;
}
// Profile management belongs to the launcher, not the restricted child. Keep
// USERENV and its initialization dependencies out of the child's import table.
class ProfileApi {
  public:
    ProfileApi() : module_(LoadLibraryExW(L"userenv.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (module_) {
            Resolve(create, "CreateAppContainerProfile");
            Resolve(remove, "DeleteAppContainerProfile");
        } else
            error = GetLastError();
    }
    ~ProfileApi() {
        if (module_)
            FreeLibrary(module_);
    }
    ProfileApi(const ProfileApi &) = delete;
    ProfileApi &operator=(const ProfileApi &) = delete;
    decltype(&CreateAppContainerProfile) create = nullptr;
    decltype(&DeleteAppContainerProfile) remove = nullptr;
    DWORD error = ERROR_SUCCESS;

  private:
    template <typename T> void Resolve(T &target, const char *name) {
        const FARPROC address = GetProcAddress(module_, name);
        if (!address && !error)
            error = GetLastError();
        static_assert(sizeof(target) == sizeof(address));
        std::memcpy(&target, &address, sizeof(target));
    }
    HMODULE module_ = nullptr;
};
class Attributes {
  public:
    ~Attributes() {
        if (list)
            DeleteProcThreadAttributeList(list);
    }
    bool Create(DWORD count = 2) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        bytes.resize(size);
        auto *candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        if (!InitializeProcThreadAttributeList(candidate, count, 0, &size))
            return false;
        list = candidate;
        return true;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    std::vector<unsigned char> bytes;
};
} // namespace
Isolation::~Isolation() {
    CloseJobAndWait();
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    RemoveContainerProfile();
    if (sid_)
        FreeSid(sid_);
}
bool Isolation::Fail(const wchar_t *stage, DWORD error) {
    stage_ = stage;
    error_ = error;
    return false;
}
bool Isolation::Create() {
    if (sid_)
        return Fail(L"already-created", ERROR_INVALID_STATE);
    ProfileApi api;
    if (!api.create || !api.remove)
        return Fail(L"profile-api-unavailable", api.error);
    static std::atomic_uint64_t next_profile{0};
    profile_ = L"Pulse.Isolation.Probe." + std::to_wstring(GetCurrentProcessId()) + L"." +
               std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++next_profile);
    const HRESULT hr =
        api.create(profile_.c_str(), L"Pulse isolation probe",
                                  L"Temporary native isolation test; no scripts", nullptr, 0, &sid_);
    if (FAILED(hr)) {
        profile_.clear(); // Never delete an existing profile we did not create.
        return Fail(L"CreateAppContainerProfile", static_cast<DWORD>(hr));
    }
    return true;
}
bool Isolation::Launch(const std::wstring &executable, const std::wstring &arguments,
                       DWORD diagnostic_memory_mib, bool diagnostic_profile_environment,
                       HANDLE standard_input, HANDLE standard_output) {
    if (!sid_ || process_)
        return Fail(L"launch-state", ERROR_INVALID_STATE);
    const bool streams = standard_input && standard_output;
    if ((standard_input != nullptr) != (standard_output != nullptr)) return Fail(L"stdio-pair", ERROR_INVALID_PARAMETER);
    HANDLE inherited[]{standard_input, standard_output};
    if (streams) {
        for (const auto handle : inherited) {
            DWORD flags = 0;
            if (!GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT) || GetFileType(handle) != FILE_TYPE_PIPE)
                return Fail(L"stdio-pipe", ERROR_INVALID_HANDLE);
        }
    }
    if (diagnostic_memory_mib && diagnostic_memory_mib != 64 && diagnostic_memory_mib != 128)
        return Fail(L"diagnostic-budget", ERROR_INVALID_PARAMETER);
    if (diagnostic_profile_environment && !diagnostic_memory_mib)
        return Fail(L"diagnostic-environment", ERROR_INVALID_PARAMETER);
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_)
        return Fail(L"CreateJobObject", GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(diagnostic_memory_mib ? diagnostic_memory_mib : 64) *
                                1024 * 1024;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Fail(L"job-memory-process-limits", GetLastError());
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu.CpuRate = 2000; // 20% hard cap; wall-clock lifetime is controlled separately.
    if (!SetInformationJobObject(job_, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)))
        return Fail(L"job-cpu-limit", GetLastError());
    Attributes attributes;
    if (!attributes.Create(streams ? 3 : 2))
        return Fail(L"attribute-list", GetLastError());
    SECURITY_CAPABILITIES security{};
    security.AppContainerSid = sid_; // No network, file, or other capability SIDs.
    DWORD children = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security,
                                   sizeof(security), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &children,
                                   sizeof(children), nullptr, nullptr))
        return Fail(L"security-attributes", GetLastError());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.list;
    if (streams) {
        if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
            return Fail(L"stdio-attributes", GetLastError());
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = standard_input;
        startup.StartupInfo.hStdOutput = standard_output;
        startup.StartupInfo.hStdError = standard_output;
    }
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + executable + L"\" " + arguments;
    // Only the two explicitly allowlisted pipe ends may cross this boundary.
    wchar_t system_root[32768]{};
    if (!GetWindowsDirectoryW(system_root, 32768))
        return Fail(L"system-root", GetLastError());
    // Windows needs the caller's LOCALAPPDATA to resolve the AppContainer profile;
    // it redirects this entry to the container's own profile during creation.
    wchar_t local_app_data[32768]{};
    const DWORD local_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, 32768);
    if (!local_length)
        return Fail(L"environment-LOCALAPPDATA", ERROR_ENVVAR_NOT_FOUND);
    if (local_length >= 32768)
        return Fail(L"environment-LOCALAPPDATA", ERROR_INSUFFICIENT_BUFFER);
    std::vector<std::wstring> entries{
        L"LOCALAPPDATA=" + std::wstring(local_app_data, local_length),
        L"SystemRoot=" + std::wstring(system_root)};
    if (diagnostic_profile_environment) {
        // Explicit diagnostic control, never an inherited environment or a security verdict.
        for (const auto *name : {L"APPDATA", L"HOMEDRIVE", L"HOMEPATH", L"ProgramData",
                                 L"SystemDrive", L"TEMP", L"TMP", L"USERPROFILE"}) {
            wchar_t value[32768]{};
            const DWORD length = GetEnvironmentVariableW(name, value, 32768);
            if (length >= 32768)
                return Fail(L"diagnostic-environment-size", ERROR_INSUFFICIENT_BUFFER);
            std::wprintf(L"[LOADER] environment %ls=%ls\n", name, length ? L"present" : L"absent");
            if (length)
                entries.push_back(std::wstring(name) + L"=" + std::wstring(value, length));
        }
        entries.push_back(L"windir=" + std::wstring(system_root));
        entries.push_back(L"PATH=" + std::wstring(system_root) + L"\\System32");
    }
    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring environment;
    for (const auto &entry : entries) {
        environment += entry;
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    // CREATE_NO_WINDOW can still launch conhost during console initialization;
    // the no-child policy correctly blocks that. This worker needs no console.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, streams ? TRUE : FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | DETACHED_PROCESS |
                            CREATE_UNICODE_ENVIRONMENT | (diagnostic_memory_mib ? DEBUG_ONLY_THIS_PROCESS : 0),
                        environment.data(), system_root, &startup.StartupInfo, &process))
        return Fail(L"CreateProcess-AppContainer", GetLastError());
    process_ = process.hProcess;
    HANDLE token = nullptr;
    DWORD is_container = 0, size = 0;
    bool valid = OpenProcessToken(process_, TOKEN_QUERY, &token) != FALSE;
    if (valid)
        valid = GetTokenInformation(token, TokenIsAppContainer, &is_container, sizeof(is_container), &size) &&
                is_container == 1;
    std::vector<unsigned char> sid_info;
    if (valid) {
        GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &size);
        sid_info.resize(size);
        valid =
            GetTokenInformation(token, TokenAppContainerSid, sid_info.data(), size, &size) &&
            EqualSid(reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(sid_info.data())->TokenAppContainer,
                     sid_);
    }
    if (token)
        CloseHandle(token);
    if (!valid || !AssignProcessToJobObject(job_, process_)) {
        const DWORD error = valid ? GetLastError() : ERROR_INVALID_TOKEN;
        TerminateProcess(process_, error);
        WaitForSingleObject(process_, 5000);
        CloseHandle(process.hThread);
        return Fail(valid ? L"AssignProcessToJobObject" : L"verify-AppContainer-token", error);
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        CloseHandle(process.hThread);
        CloseJobAndWait();
        return Fail(L"ResumeThread", error);
    }
    CloseHandle(process.hThread);
    if (diagnostic_memory_mib && !TraceLoader(process_, process.dwProcessId))
        return Fail(L"loader-diagnostic", ERROR_TIMEOUT);
    return true;
}
bool Isolation::Wait(DWORD milliseconds, DWORD &exit_code) {
    if (!process_)
        return Fail(L"wait-state", ERROR_INVALID_STATE);
    const DWORD result = WaitForSingleObject(process_, milliseconds);
    if (result != WAIT_OBJECT_0)
        return Fail(L"wait", result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    if (!GetExitCodeProcess(process_, &exit_code))
        return Fail(L"GetExitCodeProcess", GetLastError());
    stage_.clear();
    error_ = 0;
    return true;
}
bool Isolation::CloseJobAndWait() {
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    if (!process_)
        return true;
    const DWORD result = WaitForSingleObject(process_, 5000);
    return result == WAIT_OBJECT_0 ||
           Fail(L"job-reclaim", result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
}
bool Isolation::RemoveContainerProfile() {
    if (profile_.empty())
        return true;
    if (process_) {
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0)
            return Fail(L"profile-process-still-running", ERROR_BUSY);
        CloseHandle(process_);
        process_ = nullptr;
    }
    ProfileApi api;
    if (!api.remove)
        return Fail(L"profile-api-unavailable", api.error);
    const HRESULT hr = api.remove(profile_.c_str());
    if (FAILED(hr))
        return Fail(L"DeleteAppContainerProfile", static_cast<DWORD>(hr));
    profile_.clear();
    return true;
}
} // namespace pulse::extension_host
