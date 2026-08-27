#include "crash_reporter.h"
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

struct DiagnosticEvent {
    std::wstring stem;
    std::vector<std::wstring> paths;
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

void RotateDiagnostics() {
    if (g_diagnostics_root.empty()) return;
    std::map<std::wstring, DiagnosticEvent> grouped;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(g_diagnostics_root, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = data.cFileName;
        if (!name.ends_with(L".json") && !name.ends_with(L".dmp") &&
            !name.ends_with(L".log")) continue;
        const std::wstring stem = EventStemFromFile(name);
        auto& event = grouped[stem];
        event.stem = stem;
        event.paths.push_back(JoinPath(g_diagnostics_root, name));
        ULARGE_INTEGER size{data.nFileSizeLow, data.nFileSizeHigh};
        event.bytes += size.QuadPart;
        event.modified = (std::max)(event.modified, FileTimeValue(data.ftLastWriteTime));
    } while (FindNextFileW(find, &data));
    FindClose(find);

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
    for (const auto& event : events) {
        if (remaining <= kMaxEvents && total <= kMaxDiagnosticBytes) break;
        for (const auto& path : event.paths) DeleteFileW(path.c_str());
        total = total > event.bytes ? total - event.bytes : 0;
        --remaining;
    }
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
               const char* context, bool recoverable, bool dump_written) noexcept {
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
        "  \"uptime_ms\":%llu,\n  \"os\":\"%lu.%lu.%lu\",\n  \"breadcrumbs\":[",
        stem.c_str(), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds, PULSE_VERSION_STRING, PULSE_BUILD_ID,
        RoleToken(g_config.role), GetCurrentProcessId(), GetCurrentThreadId(), code,
        address, context ? context : "unknown", recoverable ? "true" : "false",
        dump_written ? "true" : "false", GetTickCount64() - g_started_tick,
        g_os_major, g_os_minor, g_os_build);
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
    const std::wstring stem = EventStem();
    const bool dump_written = WriteDump(stem, exception);
    WriteJson(stem, exception, context, recoverable, dump_written);
    if (!dump_written) WriteFallback(stem, code, address, context);
    if (recoverable) g_report_active.store(0, std::memory_order_release);
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
    g_started_tick = GetTickCount64();
    QueryOsVersion();
    RotateDiagnostics();
    g_safe_mode = DetectCrashLoop();
    g_previous_filter = SetUnhandledExceptionFilter(TopLevelFilter);
    std::set_terminate([] { ReportTerminate(); });
    g_initialized = true;
    return true;
}

void Shutdown() noexcept {
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

bool SafeModeRequested() noexcept {
    return g_safe_mode;
}

const std::wstring& DiagnosticsRoot() noexcept {
    return g_diagnostics_root;
}

} // namespace pulse::crash
