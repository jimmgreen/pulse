#include "hang_watch.h"

#include <dbghelp.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace pulse::app::hang {
namespace {

constexpr DWORD kProbeTimeoutMs = 200;  // one WM_NULL round trip
constexpr ULONGLONG kStallMs = 400;     // first sample after this long
constexpr ULONGLONG kResampleMs = 700;  // later samples while still stalled
constexpr int kMaxSamples = 6;
constexpr size_t kMaxFrames = 48;
constexpr size_t kMaxThreads = 64;

struct ThreadStack {
    DWORD tid = 0;
    size_t count = 0;
    DWORD64 frames[kMaxFrames]{};
};

std::atomic<bool> g_stop{false};
std::thread g_thread;

// Walks a suspended thread's stack from `ctx`. Touches no heap and takes no
// locks the target could hold (RtlLookupFunctionEntry reads the loader's
// inverted function table), so the suspended thread cannot deadlock us.
size_t UnwindSuspended(CONTEXT ctx, DWORD64* out, size_t cap) noexcept {
#if defined(_M_ARM64)
    (void)ctx;
    (void)out;
    (void)cap;
    return 0;
#elif defined(_M_X64)
    size_t n = 0;
    __try {
        while (n < cap && ctx.Rip) {
            out[n++] = ctx.Rip;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (!fn) {
                // Leaf function: the return address is at the top of the stack.
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx,
                                 &handler_data, &establisher, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
#else
    return 0;
#endif
}

bool SampleThread(DWORD tid, ThreadStack& out) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, tid);
    if (!th) return false;
    bool ok = false;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) {
            out.tid = tid;
            out.count = UnwindSuspended(ctx, out.frames, kMaxFrames);
            ok = out.count != 0;
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    return ok;
}

std::wstring ThreadName(DWORD tid) {
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_desc = reinterpret_cast<GetDesc>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    std::wstring name;
    if (!get_desc) return name;
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!th) return name;
    PWSTR desc = nullptr;
    if (SUCCEEDED(get_desc(th, &desc)) && desc) {
        name = desc;
        LocalFree(desc);
    }
    CloseHandle(th);
    return name;
}

std::wstring FrameText(DWORD64 addr) {
    wchar_t line[512];
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) || !mod) {
        swprintf_s(line, L"0x%016llx", static_cast<unsigned long long>(addr));
        return line;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    const DWORD64 rva = addr - reinterpret_cast<DWORD64>(mod);
    swprintf_s(line, L"%s+0x%llx", leaf, static_cast<unsigned long long>(rva));
    std::wstring text = line;
    if (mod != GetModuleHandleW(nullptr)) {
        alignas(SYMBOL_INFOW) char buf[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)]{};
        auto* sym = reinterpret_cast<SYMBOL_INFOW*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddrW(GetCurrentProcess(), addr, &disp, sym)) {
            swprintf_s(line, L"  %s+0x%llx", sym->Name, static_cast<unsigned long long>(disp));
            text += line;
        }
    }
    return text;
}

void Append(const std::wstring& log_path, const std::wstring& text) {
    HANDLE f = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(f);
}

std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void Sample(DWORD ui_tid, int stall_id, int sample, ULONGLONG elapsed, const std::wstring& log_path) {
    // Thread ids are gathered before any thread is suspended (the snapshot
    // allocates); stacks are then captured one thread at a time.
    std::vector<DWORD> tids;
    tids.push_back(ui_tid);
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL more = Thread32First(snap, &te); more && tids.size() < kMaxThreads;
             more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self &&
                te.th32ThreadID != ui_tid)
                tids.push_back(te.th32ThreadID);
        }
        CloseHandle(snap);
    }
    std::vector<ThreadStack> stacks(tids.size());
    for (size_t i = 0; i < tids.size(); ++i) SampleThread(tids[i], stacks[i]);

    SymRefreshModuleList(GetCurrentProcess());
    wchar_t head[160];
    swprintf_s(head, L"=== stall #%d sample %d at +%llu ms (%s)\r\n", stall_id, sample,
               static_cast<unsigned long long>(elapsed), Timestamp().c_str());
    std::wstring text = head;
    for (size_t i = 0; i < stacks.size(); ++i) {
        const ThreadStack& st = stacks[i];
        if (!st.count) continue;
        wchar_t th[96];
        swprintf_s(th, L"-- thread %lu%s ", st.tid, i == 0 ? L" [UI]" : L"");
        text += th;
        text += ThreadName(st.tid);
        text += L"\r\n";
        for (size_t f = 0; f < st.count; ++f) {
            wchar_t idx[16];
            swprintf_s(idx, L"  #%02zu ", f);
            text += idx;
            text += FrameText(st.frames[f]);
            text += L"\r\n";
        }
    }
    Append(log_path, text);
}

void Run(HWND hwnd, std::wstring log_path) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitializeW(GetCurrentProcess(), nullptr, TRUE);
    const DWORD ui_tid = GetWindowThreadProcessId(hwnd, nullptr);
    Append(log_path, L"### hang watch started " + Timestamp() + L"\r\n");
    bool stalled = false;
    ULONGLONG stall_start = 0, next_sample = 0;
    int stall_id = 0, samples = 0;
    while (!g_stop.load()) {
        const ULONGLONG t0 = GetTickCount64();
        DWORD_PTR result = 0;
        SetLastError(0);
        const LRESULT ok = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, kProbeTimeoutMs, &result);
        const DWORD err = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (ok) {
            if (stalled) {
                wchar_t line[128];
                swprintf_s(line, L"--- stall #%d ended after %llu ms (%s)\r\n\r\n", stall_id,
                           static_cast<unsigned long long>(now - stall_start), Timestamp().c_str());
                if (samples > 0) Append(log_path, line);
                stalled = false;
            }
            Sleep(100);
            continue;
        }
        if (err != ERROR_TIMEOUT && err != 0) break;  // window gone
        if (!stalled) {
            stalled = true;
            stall_start = t0;
            next_sample = t0 + kStallMs;
            samples = 0;
            ++stall_id;
        }
        if (now >= next_sample && samples < kMaxSamples) {
            Sample(ui_tid, stall_id, ++samples, now - stall_start, log_path);
            next_sample = GetTickCount64() + kResampleMs;
        }
    }
    SymCleanup(GetCurrentProcess());
}

} // namespace

void Start(HWND hwnd, const std::wstring& log_path) {
    if (g_thread.joinable() || !hwnd) return;
    g_stop = false;
    g_thread = std::thread(Run, hwnd, log_path);
}

void Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

// Explicit in-process probes reuse the allocation-free suspended stack walker.
std::wstring ThreadStackForProbe(DWORD tid) {
    if (tid == GetCurrentThreadId()) return {};
    ThreadStack stack;
    if (!SampleThread(tid, stack)) return {};
    std::wstring text;
    for (size_t i = 0; i < stack.count; ++i) text += FrameText(stack.frames[i]) + L"\n";
    return text;
}

} // namespace pulse::app::hang
