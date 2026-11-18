#include "crash_reporter.h"
#include "runtime_log.h"
#include "pulse_version.h"

#include <dbghelp.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <intrin.h>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace pulse::crash {
namespace {

constexpr size_t kBreadcrumbCount = 64;
constexpr size_t kMaxEvents = 10;
constexpr uint64_t kMaxDiagnosticBytes = 200ull * 1024ull * 1024ull;
constexpr uint64_t kSafeModeWindow100ns = 10ull * 60ull * 10000000ull;

struct Breadcrumb {
    std::atomic<uint64_t> sequence{0};
    uint64_t tick = 0;
    uint32_t category = 0;
    uint32_t action = 0;
    int32_t result = 0;
};

struct ScopedHandle {
    HANDLE value;
    ~ScopedHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct ScopedFind {
    HANDLE value;
    ~ScopedFind() { FindClose(value); }
};

struct DiagnosticEvent {
    std::wstring stem;
    std::vector<std::pair<std::wstring, uint64_t>> paths;
    uint64_t bytes = 0;
    uint64_t modified = 0;
};

Config g_config;
std::wstring g_diagnostics_root;
std::array<Breadcrumb, kBreadcrumbCount> g_breadcrumbs;
std::atomic<uint64_t> g_breadcrumb_sequence{0};
std::atomic<long> g_report_active{0};
std::atomic<long> g_event_counter{0};
ULONGLONG g_started_tick = 0;
DWORD g_os_major = 0;
DWORD g_os_minor = 0;
DWORD g_os_build = 0;
bool g_initialized = false;
bool g_safe_mode = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
std::atomic<FaultAttribution> g_attribution{nullptr};
struct Maintenance {
    std::wstring root;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = nullptr;
    ~Maintenance() { if (wake) CloseHandle(wake); if (stop) CloseHandle(stop); if (thread) CloseHandle(thread); }
};
SRWLOCK g_maintenance_lock = SRWLOCK_INIT;
std::shared_ptr<Maintenance> g_maintenance;
// Keep the exception path allocation-free and nonblocking for maintenance.
void RequestMaintenance() noexcept {
    if (!TryAcquireSRWLockShared(&g_maintenance_lock)) return;
    if (g_maintenance) SetEvent(g_maintenance->wake);
    ReleaseSRWLockShared(&g_maintenance_lock);
}


uint64_t FileTimeValue(const FILETIME& value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::wstring JoinPath(std::wstring_view left, std::wstring_view right) {
    std::wstring value(left);
    if (!value.empty() && value.back() != L'\\') value.push_back(L'\\');
    value.append(right);
    return value;
}

bool EnsureDirectory(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::wstring DefaultDataRoot(bool machine_scope) {
    wchar_t path[MAX_PATH]{};
    const int csidl = machine_scope ? CSIDL_COMMON_APPDATA : CSIDL_LOCAL_APPDATA;
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path))) return {};
    return JoinPath(path, L"Pulse");
}

std::wstring SafeBuildId() {
    std::wstring value = PULSE_BUILD_ID;
    for (wchar_t& c : value) {
        if (!iswalnum(c) && c != L'-' && c != L'_') c = L'_';
    }
    return value;
}

const wchar_t* RoleToken(ProcessRole role) noexcept {
    switch (role) {
    case ProcessRole::App: return L"app";
    case ProcessRole::IndexService: return L"index-service";
    case ProcessRole::IndexHelper: return L"index-helper";
    case ProcessRole::NetworkAgent: return L"network-agent";
    case ProcessRole::ContentAgent: return L"content-agent";
    case ProcessRole::Preview: return L"preview";
    case ProcessRole::Shell: return L"shell";
    case ProcessRole::Test: return L"test";
    }
    return L"unknown";
}

std::wstring EventStem() noexcept {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t value[256]{};
    swprintf_s(value, L"%04u%02u%02uT%02u%02u%02u.%03uZ-%s-%s-%lu-%lu-%ld",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, now.wMilliseconds, RoleToken(g_config.role),
               SafeBuildId().c_str(), GetCurrentProcessId(), GetCurrentThreadId(),
               g_event_counter.fetch_add(1, std::memory_order_relaxed) + 1);
    return value;
}

std::wstring EventStemFromFile(std::wstring name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

// Returns true when the sweep left work behind (a report still being written,
// a file someone holds open): the maintenance thread tries again later. A
// sweep another process is running already counts as done.
bool RotateDiagnostics(const std::wstring& root, HANDLE stop = nullptr) {
    if (root.empty()) return false;
    ScopedHandle retention_lock{CreateFileW(JoinPath(root, L"retention.lock").c_str(),
        GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
    if (retention_lock.value == INVALID_HANDLE_VALUE) return false;
    std::map<std::wstring, DiagnosticEvent> grouped;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(root, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    ScopedFind find_guard{find};
    do {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring name = data.cFileName;
        if (!name.ends_with(L".json") && !name.ends_with(L".dmp") &&
            !name.ends_with(L".log")) continue;
        const std::wstring stem = EventStemFromFile(name);
        auto& event = grouped[stem];
        event.stem = stem;
        ULARGE_INTEGER size{data.nFileSizeLow, data.nFileSizeHigh};
        event.bytes += size.QuadPart;
        event.modified = (std::max)(event.modified, FileTimeValue(data.ftLastWriteTime));
    } while (FindNextFileW(find, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) return true;

    std::vector<DiagnosticEvent> events;
    events.reserve(grouped.size());
    uint64_t total = 0;
    for (auto& [key, event] : grouped) {
        total += event.bytes;
        events.push_back(std::move(event));
    }
    std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        return left.modified < right.modified;
    });
    size_t remaining = events.size();
    bool pending = false;
    for (const auto& event : events) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
        // Keep the newest report even when it alone exceeds the byte budget.
        if (&event == &events.back()) break;
        if (remaining <= kMaxEvents && total <= kMaxDiagnosticBytes) break;
        const std::wstring lock_path = JoinPath(root, event.stem + L".writing");
        HANDLE lock = CreateFileW(lock_path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock == INVALID_HANDLE_VALUE) { pending = true; continue; }
        ScopedHandle event_guard{lock};
        // Refresh the group after taking its lease: enumeration may have overlapped
        // a writer that closed its dump before creating the metadata.
        std::vector<std::wstring> paths;
        WIN32_FIND_DATAW current{};
        HANDLE scan = FindFirstFileW(JoinPath(root, event.stem + L".*").c_str(), &current);
        if (scan == INVALID_HANDLE_VALUE) continue;
        ScopedFind scan_guard{scan};
        do {
            if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
            const std::wstring_view name(current.cFileName);
            if (!(current.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                (name.ends_with(L".json") || name.ends_with(L".dmp") || name.ends_with(L".log")))
                paths.push_back(JoinPath(root, name));
        } while (FindNextFileW(scan, &current));
        const DWORD scan_error = GetLastError();
        if (scan_error != ERROR_NO_MORE_FILES) { pending = true; continue; }
        bool removed = true;
        for (const auto& path : paths) {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            const bool measured = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE;
            if (DeleteFileW(path.c_str())) {
                if (measured) {
                    const uint64_t bytes = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) |
                        attributes.nFileSizeLow;
                    total -= (std::min)(total, bytes);
                }
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND) removed = false;
        }
        if (removed) --remaining;
        else pending = true;
    }
    return pending;
}

DWORD WINAPI MaintenanceMain(void* argument) {
    std::unique_ptr<std::shared_ptr<Maintenance>> holder(static_cast<std::shared_ptr<Maintenance>*>(argument));
    auto state = *holder; holder.reset();
    const HANDLE events[]{state->stop, state->wake};
    // Sleep until a report asks for a sweep. Only a sweep that left files
    // behind is retried without one, backing off from 1 s to a minute: polling
    // every second re-created retention.lock in every Pulse process forever.
    DWORD retry_ms = 0;
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, retry_ms ? retry_ms : INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        bool pending = false;
        try { pending = RotateDiagnostics(state->root, state->stop); } catch (...) {}
        retry_ms = !pending ? 0 : retry_ms ? (std::min)(retry_ms * 2, DWORD{60000}) : 1000;
    }
    return 0;
}
void StartMaintenance() noexcept {
    try {
        auto state = std::make_shared<Maintenance>(); state->root = g_diagnostics_root;
        if (!state->wake || !state->stop) return;
        auto* argument = new std::shared_ptr<Maintenance>(state);
        state->thread = CreateThread(nullptr, 0, MaintenanceMain, argument, 0, nullptr);
        if (!state->thread) { delete argument; return; }
        AcquireSRWLockExclusive(&g_maintenance_lock); g_maintenance = std::move(state);
        ReleaseSRWLockExclusive(&g_maintenance_lock);
    } catch (...) {}
}

bool DetectCrashLoop() {
    if (g_config.role != ProcessRole::App || g_diagnostics_root.empty()) return false;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = FileTimeValue(now_ft);
    const std::wstring marker = L"-app-" + SafeBuildId() + L"-";
    size_t count = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(g_diagnostics_root, L"*.json").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        const uint64_t modified = FileTimeValue(data.ftLastWriteTime);
        if (std::wstring_view(data.cFileName).find(marker) != std::wstring_view::npos &&
            now >= modified && now - modified <= kSafeModeWindow100ns) ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count >= 3;
}

void QueryOsVersion() noexcept {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtl_get_version = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version && rtl_get_version(&version) == 0) {
        g_os_major = version.dwMajorVersion;
        g_os_minor = version.dwMinorVersion;
        g_os_build = version.dwBuildNumber;
    }
}

void WriteFallback(const std::wstring& stem, DWORD code, void* address,
                   const char* context) noexcept {
    const std::wstring path = JoinPath(g_diagnostics_root, stem + L".log");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[1024]{};
    const int length = snprintf(line, sizeof(line),
        "context=%s code=0x%08lX address=%p pid=%lu tid=%lu version=%ls build=%ls\r\n",
        context ? context : "unknown", code, address, GetCurrentProcessId(),
        GetCurrentThreadId(), PULSE_VERSION_STRING, PULSE_BUILD_ID);
    if (length > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>((std::min)(length,
                  static_cast<int>(sizeof(line) - 1))), &written, nullptr);
    }
    CloseHandle(file);
}

bool WriteDump(const std::wstring& stem, EXCEPTION_POINTERS* exception) noexcept {
    const std::wstring path = JoinPath(g_diagnostics_root, stem + L".dmp");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal |
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      type, exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok != FALSE;
}

void WriteJson(const std::wstring& stem, EXCEPTION_POINTERS* exception,
               const char* context, bool recoverable, bool dump_written,
               const char* extension) noexcept {
    const std::wstring path = JoinPath(g_diagnostics_root, stem + L".json");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD code = exception && exception->ExceptionRecord
        ? exception->ExceptionRecord->ExceptionCode : 0xE0000001u;
    const void* address = exception && exception->ExceptionRecord
        ? exception->ExceptionRecord->ExceptionAddress : nullptr;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char buffer[16384]{};
    int length = snprintf(buffer, sizeof(buffer),
        "{\n  \"schema\":1,\n  \"event_id\":\"%ls\",\n"
        "  \"utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\n"
        "  \"version\":\"%ls\",\n  \"build_id\":\"%ls\",\n"
        "  \"role\":\"%ls\",\n  \"pid\":%lu,\n  \"tid\":%lu,\n"
        "  \"exception_code\":\"0x%08lX\",\n  \"exception_address\":\"%p\",\n"
        "  \"context\":\"%s\",\n  \"recoverable\":%s,\n  \"dump_written\":%s,\n"
        "  \"uptime_ms\":%llu,\n  \"os\":\"%lu.%lu.%lu\",\n  \"extension\":\"%s\",\n"
        "  \"breadcrumbs\":[",
        stem.c_str(), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds, PULSE_VERSION_STRING, PULSE_BUILD_ID,
        RoleToken(g_config.role), GetCurrentProcessId(), GetCurrentThreadId(), code,
        address, context ? context : "unknown", recoverable ? "true" : "false",
        dump_written ? "true" : "false", GetTickCount64() - g_started_tick,
        g_os_major, g_os_minor, g_os_build, extension ? extension : "");
    const uint64_t newest = g_breadcrumb_sequence.load(std::memory_order_acquire);
    const uint64_t first = newest > kBreadcrumbCount ? newest - kBreadcrumbCount + 1 : 1;
    bool emitted = false;
    for (uint64_t sequence = first; sequence <= newest && length > 0; ++sequence) {
        const Breadcrumb& item = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
        if (item.sequence.load(std::memory_order_acquire) != sequence) continue;
        length += snprintf(buffer + length, sizeof(buffer) - static_cast<size_t>(length),
            "%s{\"seq\":%llu,\"tick\":%llu,\"category\":%u,\"action\":%u,\"result\":%d}",
            emitted ? "," : "", sequence, item.tick, item.category, item.action, item.result);
        emitted = true;
        if (length >= static_cast<int>(sizeof(buffer) - 128)) break;
    }
    if (length > 0 && length < static_cast<int>(sizeof(buffer) - 8))
        length += snprintf(buffer + length, sizeof(buffer) - static_cast<size_t>(length), "]\n}\n");
    if (length > 0) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>((std::min)(length,
                  static_cast<int>(sizeof(buffer) - 1))), &written, nullptr);
    }
    CloseHandle(file);
}

LONG WriteReport(EXCEPTION_POINTERS* exception, const char* context,
                 bool recoverable) noexcept {
    if (!g_initialized || g_diagnostics_root.empty())
        return recoverable ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_EXECUTE_HANDLER;
    long expected = 0;
    if (!g_report_active.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return EXCEPTION_EXECUTE_HANDLER;
    const DWORD code = exception && exception->ExceptionRecord
        ? exception->ExceptionRecord->ExceptionCode : 0xE0000001u;
    void* address = exception && exception->ExceptionRecord
        ? exception->ExceptionRecord->ExceptionAddress : nullptr;
    // Attribute first: the hook records the plugin for the next start even if
    // the dump below runs out of stack or the process is killed mid-write.
    const char* extension = nullptr;
    if (!recoverable) {
        if (const FaultAttribution attribution = g_attribution.load(std::memory_order_acquire))
            extension = attribution(address);
    }
    const std::wstring stem = EventStem();
    HANDLE lease = CreateFileW(JoinPath(g_diagnostics_root, stem + L".writing").c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lease == INVALID_HANDLE_VALUE) {
        if (recoverable) g_report_active.store(0, std::memory_order_release);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    const bool dump_written = WriteDump(stem, exception);
    WriteJson(stem, exception, context, recoverable, dump_written, extension);
    if (!dump_written) WriteFallback(stem, code, address, context);
    if (lease != INVALID_HANDLE_VALUE) CloseHandle(lease);
    if (recoverable) {
        g_report_active.store(0, std::memory_order_release);
        RequestMaintenance();
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* exception) noexcept {
    return ReportFatal(exception, "unhandled");
}

} // namespace

const wchar_t* RoleName(ProcessRole role) noexcept {
    return RoleToken(role);
}

bool Initialize(const Config& config) noexcept {
    if (g_initialized) return true;
    g_config = config;
    std::wstring root = config.data_root.empty() ? DefaultDataRoot(config.machine_scope)
                                                  : config.data_root;
    if (root.empty() || !EnsureDirectory(root)) return false;
    const std::wstring diagnostics = JoinPath(root, L"Diagnostics");
    if (!EnsureDirectory(diagnostics)) return false;
    g_diagnostics_root = JoinPath(diagnostics, L"Crashes");
    if (!EnsureDirectory(g_diagnostics_root)) {
        g_diagnostics_root.clear();
        return false;
    }
    const std::wstring role = RoleToken(config.role);
    std::string component;
    for (wchar_t c : role) component.push_back(static_cast<char>(c));
    (void)diagnostics::runtime::Initialize(root, component.c_str());
    g_started_tick = GetTickCount64();
    QueryOsVersion();
    const bool rotation_pending = RotateDiagnostics(g_diagnostics_root);
    StartMaintenance();
    if (rotation_pending) RequestMaintenance();
    g_safe_mode = DetectCrashLoop();
    g_previous_filter = SetUnhandledExceptionFilter(TopLevelFilter);
    std::set_terminate([] { ReportTerminate(); });
    g_initialized = true;
    return true;
}

void Shutdown() noexcept {
    AcquireSRWLockExclusive(&g_maintenance_lock);
    if (g_maintenance) SetEvent(g_maintenance->stop);
    g_maintenance.reset(); // Worker owns its state; never join filesystem I/O here.
    ReleaseSRWLockExclusive(&g_maintenance_lock);
    diagnostics::runtime::Shutdown();
    if (!g_initialized) return;
    SetUnhandledExceptionFilter(g_previous_filter);
    g_initialized = false;
}

void AddBreadcrumb(uint32_t category, uint32_t action, int32_t result) noexcept {
    const uint64_t sequence = g_breadcrumb_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    Breadcrumb& item = g_breadcrumbs[(sequence - 1) % kBreadcrumbCount];
    item.sequence.store(0, std::memory_order_release);
    item.tick = GetTickCount64();
    item.category = category;
    item.action = action;
    item.result = result;
    item.sequence.store(sequence, std::memory_order_release);
}

LONG ReportFatal(EXCEPTION_POINTERS* exception, const char* context) noexcept {
    return WriteReport(exception, context, false);
}

LONG ReportRecoverable(EXCEPTION_POINTERS* exception, const char* context) noexcept {
    return WriteReport(exception, context, true);
}

[[noreturn]] void ReportTerminate(const char* context) noexcept {
    CONTEXT cpu{};
    RtlCaptureContext(&cpu);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000001u;
    record.ExceptionAddress = _ReturnAddress();
    EXCEPTION_POINTERS pointers{&record, &cpu};
    WriteReport(&pointers, context, false);
    TerminateProcess(GetCurrentProcess(), 3);
    __assume(false);
}

void SetFaultAttribution(FaultAttribution attribution) noexcept {
    g_attribution.store(attribution, std::memory_order_release);
}

bool SafeModeRequested() noexcept {
    return g_safe_mode;
}

const std::wstring& DiagnosticsRoot() noexcept {
    return g_diagnostics_root;
}

} // namespace pulse::crash
