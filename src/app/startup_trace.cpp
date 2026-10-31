#include "startup_trace.h"

#include <winternl.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

// g_crt_init_mark (below) runs before the other static constructors.
#pragma warning(push)
#pragma warning(disable : 4073)  // initializers put in library initialization area
#pragma init_seg(lib)
#pragma warning(pop)

namespace pulse::app::startup_trace {
namespace {

constexpr size_t kMaxMarks = 256;
constexpr size_t kMaxDlls = 160;
constexpr size_t kMaxStaticModules = 256;
constexpr size_t kMaxMessages = 64;
constexpr double kSlowMessageMs = 2.0;
constexpr UINT_PTR kFlushTimerId = 0x50545243;  // 'PTRC'

struct MarkEntry {
    const char* label;
    int occurrence;
    double ms;
    DWORD thread;
};

struct DllEntry {
    wchar_t name[48];
    double ms;
    DWORD thread;
};

struct MessageEntry {
    UINT msg;
    double start;
    double duration;
};

// ntdll's loader notification (documented in LdrRegisterDllNotification).
struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* full_name;
    const UNICODE_STRING* base_name;
    void* base;
    ULONG size;
};
using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn fn, PVOID context, PVOID* cookie);
constexpr ULONG kDllLoaded = 1;

int g_state = 0;  // 0 unknown, 1 enabled, -1 disabled; set on the UI thread
ULONGLONG g_created = 0;
DWORD g_main_thread = 0;
struct CounterEntry { const char* label; double value; };
CounterEntry g_counters[32];
size_t g_counter_count = 0;
MarkEntry g_marks[kMaxMarks];
size_t g_mark_count = 0;
DllEntry g_dlls[kMaxDlls];
std::atomic<size_t> g_dll_count{0};
HMODULE g_static_modules[kMaxStaticModules];
DWORD g_static_module_count = 0;
MessageEntry g_messages[kMaxMessages];
size_t g_message_count = 0;
bool g_flushed = false;
SRWLOCK g_lock = SRWLOCK_INIT;

ULONGLONG NowFileTime() noexcept {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

double SinceCreation() noexcept {
    return static_cast<double>(NowFileTime() - g_created) / 10000.0;
}

VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, PVOID) {
    // Runs under the loader lock: no allocation, no further loads.
    if (reason != kDllLoaded || !data || !data->base_name) return;
    const size_t slot = g_dll_count.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kMaxDlls) return;
    DllEntry& entry = g_dlls[slot];
    const size_t chars = (std::min)(static_cast<size_t>(data->base_name->Length / sizeof(wchar_t)),
                                    ARRAYSIZE(entry.name) - 1);
    std::memcpy(entry.name, data->base_name->Buffer, chars * sizeof(wchar_t));
    entry.name[chars] = L'\0';
    entry.ms = SinceCreation();
    entry.thread = GetCurrentThreadId();
}

bool Enabled() noexcept {
    if (g_state != 0) return g_state > 0;
    wchar_t value[4]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_STARTUP_TRACE", value, ARRAYSIZE(value));
    if (n == 0 || n >= ARRAYSIZE(value) || value[0] == L'0') {
        g_state = -1;
        return false;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    g_created = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    g_main_thread = GetCurrentThreadId();
    g_state = 1;
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), g_static_modules, sizeof(g_static_modules), &bytes))
        g_static_module_count = (std::min)(bytes / static_cast<DWORD>(sizeof(HMODULE)),
                                           static_cast<DWORD>(kMaxStaticModules));
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto reg = reinterpret_cast<RegisterFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrRegisterDllNotification")));
        PVOID cookie = nullptr;
        if (reg) reg(0, OnDllNotification, nullptr, &cookie);  // kept for the process lifetime
    }
    return true;
}

void Flush() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_lock); } } unlock;
    if (g_flushed) return;
    g_flushed = true;
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 32) return;
    wcscat_s(path, L"pulse_startup_trace.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"# Pulse startup trace: ms since process creation; main thread %lu\n", g_main_thread);
    double prev = 0.0;
    for (size_t i = 0; i < g_mark_count; ++i) {
        const MarkEntry& m = g_marks[i];
        const bool other = m.thread != g_main_thread;
        if (m.occurrence > 1)
            fwprintf(f, L"%9.2f  +%8.2f  %hs#%d%ls\n", m.ms, m.ms - prev, m.label, m.occurrence,
                     other ? L"  [worker]" : L"");
        else
            fwprintf(f, L"%9.2f  +%8.2f  %hs%ls\n", m.ms, m.ms - prev, m.label, other ? L"  [worker]" : L"");
        if (!other) prev = m.ms;
    }
    fwprintf(f, L"\n# Cumulative diagnostic counters at latest frame\n");
    for (size_t i = 0; i < g_counter_count; ++i)
        fwprintf(f, L"%hs = %.3f\n", g_counters[i].label, g_counters[i].value);
    fwprintf(f, L"\n# Window messages taking %.0f ms or more (start, duration)\n", kSlowMessageMs);
    for (size_t i = 0; i < g_message_count; ++i)
        fwprintf(f, L"%9.2f  %8.2f  msg 0x%04X\n", g_messages[i].start, g_messages[i].duration, g_messages[i].msg);
    fwprintf(f, L"\n# Modules loaded before the first mark (%lu)\n", g_static_module_count);
    for (DWORD i = 0; i < g_static_module_count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (K32GetModuleBaseNameW(GetCurrentProcess(), g_static_modules[i], name, MAX_PATH))
            fwprintf(f, L"  %ls\n", name);
    }
    const size_t dlls = (std::min)(g_dll_count.load(std::memory_order_relaxed), kMaxDlls);
    fwprintf(f, L"\n# DLLs loaded after the first mark (%zu)\n", dlls);
    for (size_t i = 0; i < dlls; ++i)
        fwprintf(f, L"%9.2f  %ls%ls\n", g_dlls[i].ms, g_dlls[i].name,
                 g_dlls[i].thread != g_main_thread ? L"  [worker]" : L"");
    fclose(f);
}

VOID CALLBACK OnFlushTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    Flush();
}

// Earliest point the CRT reaches in this image: the gap from process creation
// is the loader's work on the static imports, and from here to wwinmain the
// C++ static constructors.
struct CrtInitMark {
    CrtInitMark() noexcept { Mark("crt_init"); }
} g_crt_init_mark;

}  // namespace

void Mark(const char* label) noexcept {
    if (!Enabled()) return;
    // Workers may mark too (tagged [worker]); the first Mark comes from
    // wWinMain, so Enabled() itself is never raced.
    AcquireSRWLockExclusive(&g_lock);
    int occurrence = 1;
    for (size_t i = 0; i < g_mark_count; ++i)
        if (g_marks[i].label == label) ++occurrence;
    if (occurrence <= 3 && g_mark_count < kMaxMarks)
        g_marks[g_mark_count++] = {label, occurrence, SinceCreation(), GetCurrentThreadId()};
    ReleaseSRWLockExclusive(&g_lock);
}

void Counter(const char* label, double value) noexcept {
    if (!Enabled()) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_flushed) {
        size_t i = 0;
        while (i < g_counter_count && g_counters[i].label != label) ++i;
        if (i < ARRAYSIZE(g_counters)) {
            g_counters[i] = {label, value};
            if (i == g_counter_count) ++g_counter_count;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void ScheduleFlush(HWND hwnd, UINT delay_ms) noexcept {
    if (!Enabled() || g_flushed) return;
    if (!hwnd || !SetTimer(hwnd, kFlushTimerId, delay_ms, OnFlushTimer)) Flush();
}

MessageScope::MessageScope(UINT msg) noexcept : msg_(msg) {
    if (g_state > 0 && !g_flushed && GetCurrentThreadId() == g_main_thread) start_ = SinceCreation();
}

MessageScope::~MessageScope() {
    if (start_ < 0.0 || g_flushed) return;
    const double duration = SinceCreation() - start_;
    if (duration < kSlowMessageMs) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_message_count < kMaxMessages) g_messages[g_message_count++] = {msg_, start_, duration};
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace pulse::app::startup_trace
