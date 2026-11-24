#include "hang_watch.h"
#include "../common/runtime_log.h"
#include <array>
#include <mutex>

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
    size_t n = 0;
    __try {
#if defined(_M_X64)
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
#elif defined(_M_IX86)
        // x86 optimized code may omit frame pointers. Always keep the sampled
        // instruction; follow only a bounded, monotonic EBP chain when present.
        if (cap && ctx.Eip) out[n++] = ctx.Eip;
        DWORD frame = ctx.Ebp;
        while (n < cap && frame >= ctx.Esp && !(frame & 3) && frame - ctx.Esp < 1024 * 1024) {
            const auto* words = reinterpret_cast<const DWORD*>(static_cast<uintptr_t>(frame));
            const DWORD next = words[0], caller = words[1];
            if (!caller || next <= frame) break;
            out[n++] = caller;
            frame = next;
        }
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
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

std::wstring FrameText(DWORD64 addr, bool symbols = true) {
    wchar_t line[512];
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &mod) || !mod) {
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
    if (symbols && mod != GetModuleHandleW(nullptr)) {
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


std::atomic<uint64_t> g_frame_id{0}, g_frame_start{0};
std::atomic<uint64_t> g_dispatch_id{0}, g_dispatch_start{0}, g_dispatch_heartbeat{0};
std::atomic<UINT> g_dispatch_msg{0};
std::atomic<int> g_modal_depth{0};
std::atomic<bool> g_frame_enabled{false};
uint64_t g_frame_sequence = 0; // UI-thread owned; never reused across restarts.
uint64_t g_dispatch_sequence = 0; // UI-thread owned; never reused across restarts.
std::thread g_frame_thread;
HANDLE g_frame_stop = nullptr;
ULONGLONG g_frame_recent[3]{};
size_t g_frame_recent_count = 0, g_frame_recent_next = 0;
size_t g_frame_total = 0; // Worker-owned, process lifetime budget.
size_t g_dispatch_total = 0; // Worker-owned, process lifetime budget.
constexpr LONGLONG kFrameLogLimit = 2 * 1024 * 1024;
constexpr ULONGLONG kDispatchStallMs = 400;
constexpr ULONGLONG kDispatchResampleMs = 700;

struct OperationContext {
    uint64_t task = 0, phase = 0, completed = 0, total = 0, updated = 0;
};
struct DispatchRecord {
    uint64_t id = 0, message = 0, elapsed = 0, modal = 0, nested = 0, ended = 0;
    OperationContext context;
};
struct ProbeCorrelation {
    uint64_t dispatch = 0, message = 0, modal = 0, nested = 0;
    OperationContext context;
};
struct ProbeState {
    HWND window = nullptr;
    uint64_t sequence = 0, pending = 0, sent_tick = 0, ack_tick = 0, next_post = 0;
    bool late = false, sampled = false, config_due = true;
    ProbeCorrelation correlation;
    uint64_t sent = 0, acked = 0, late_count = 0, post_failures = 0, post_error = 0, maximum = 0;
    uint64_t abandoned = 0, rejected_acks = 0;
    uint64_t last_health = 0, slow_window = 0, slow_logged = 0, slow_dropped = 0;
    size_t slow_in_window = 0, head = 0, count = 0;
    std::array<DispatchRecord, 64> slow{};
    OperationContext context;
};
std::mutex g_probe_mutex;
ProbeState g_probe;
std::atomic<bool> g_dispatch_nested{false}, g_dispatch_modal{false};
size_t g_probe_stack_total = 0; // Process-lifetime stack budget, monitor owned.

uint64_t ContextAge(const OperationContext& context, uint64_t now) {
    return context.updated && now >= context.updated ? now - context.updated : UINT64_MAX;
}
void LogProbe(const char* name, uint64_t token, uint64_t elapsed, const ProbeCorrelation& correlation) {
    const auto& context = correlation.context;
    diagnostics::runtime::Event(name, {{"token", token}, {"elapsed_ms", elapsed},
        {"dispatch_id", correlation.dispatch}, {"message", correlation.message},
        {"task", context.task}, {"phase", context.phase}, {"completed", context.completed},
        {"total", context.total}, {"context_age_ms", ContextAge(context, GetTickCount64())},
        {"modal", correlation.modal}, {"nested", correlation.nested}});
}

void MarkProbeCovered(uint64_t sampled_tick) {
    std::lock_guard lock(g_probe_mutex);
    if (g_probe.pending && g_probe.sent_tick <= sampled_tick) g_probe.sampled = true;
}
void PollDiagnostics(bool final = false) {
    const uint64_t now = GetTickCount64();
    bool late = false, recovered = false, health = false, config = false;
    uint64_t token = 0, elapsed = 0;
    ProbeCorrelation correlation;
    ProbeState snapshot;
    std::array<DispatchRecord, 4> records{};
    size_t record_count = 0;
    {
        std::lock_guard lock(g_probe_mutex);
        if (g_probe.pending && g_probe.window && !IsWindow(g_probe.window)) { ++g_probe.abandoned; g_probe.pending = 0; }
        if (g_probe.pending) {
            token = g_probe.pending;
            elapsed = (g_probe.ack_tick ? g_probe.ack_tick : now) - g_probe.sent_tick;
            if (elapsed >= 150 && !g_probe.late) {
                g_probe.late = true; ++g_probe.late_count; late = true;
                const auto dispatch = g_dispatch_id.load(std::memory_order_acquire);
                const auto message = g_dispatch_msg.load();
                const bool current = dispatch == g_dispatch_id.load(std::memory_order_acquire);
                g_probe.correlation = {current ? dispatch : 0, current && dispatch ? message : 0,
                    static_cast<uint64_t>(g_modal_depth.load() > 0), g_dispatch_nested.load(), g_probe.context};
            }
            correlation = g_probe.correlation;
            if (g_probe.ack_tick) {
                recovered = g_probe.late;
                g_probe.pending = 0; g_probe.ack_tick = 0;
                g_probe.next_post = now + 500;
            }
        }
        if (!final && g_probe.window && !g_probe.pending && now >= g_probe.next_post) {
            g_probe.pending = ++g_probe.sequence;
            g_probe.sent_tick = now; g_probe.ack_tick = 0; g_probe.late = false; g_probe.sampled = false;
            g_probe.next_post = now + 500;
            if (PostMessageW(g_probe.window, UiProbeMessage(), static_cast<WPARAM>(g_probe.pending), 0)) ++g_probe.sent;
            else { ++g_probe.post_failures; g_probe.post_error = GetLastError(); g_probe.pending = 0; }
        }
        if (now - g_probe.slow_window >= 1000) { g_probe.slow_window = now; g_probe.slow_in_window = 0; }
        while (g_probe.count && record_count < records.size()) {
            const auto record = g_probe.slow[g_probe.head];
            g_probe.head = (g_probe.head + 1) % g_probe.slow.size(); --g_probe.count;
            if (g_probe.slow_in_window >= 4) { ++g_probe.slow_dropped; continue; }
            ++g_probe.slow_in_window; ++g_probe.slow_logged;
            records[record_count++] = record;
        }
        config = g_probe.config_due; g_probe.config_due = false;
        health = final || now - g_probe.last_health >= 5000;
        if (health) g_probe.last_health = now;
        snapshot = g_probe;
    }
    if (config) diagnostics::runtime::Event("ui_probe_config", {{"schema", 1}, {"probe_message", UiProbeMessage()},
        {"period_ms", 500}, {"late_threshold_ms", 150}, {"slow_dispatch_ms", 100}, {"stack_threshold_ms", 400},
        {"health_period_ms", 5000}, {"frame_stack_budget", 100}, {"dispatch_stack_budget", 60}, {"probe_stack_budget", 20}});
    if (late) LogProbe("ui_probe_late", token, elapsed, correlation);
    if (recovered) LogProbe("ui_probe_recovered", token, elapsed, correlation);
    for (size_t i = 0; i < record_count; ++i) {
        const auto& r = records[i];
        diagnostics::runtime::Event("ui_slow_dispatch", {{"dispatch_id", r.id}, {"message", r.message},
            {"elapsed_ms", r.elapsed}, {"modal", r.modal}, {"nested", r.nested},
            {"task", r.context.task}, {"phase", r.context.phase}, {"completed", r.context.completed},
            {"total", r.context.total}, {"context_age_ms", ContextAge(r.context, r.ended)}});
    }
    if (health) {
        diagnostics::runtime::Event("ui_probe_health", {{"sent", snapshot.sent}, {"acked", snapshot.acked},
            {"late", snapshot.late_count}, {"post_failures", snapshot.post_failures}, {"post_error", snapshot.post_error},
            {"max_latency_ms", snapshot.maximum}, {"pending_ms", snapshot.pending ? now - snapshot.sent_tick : 0},
            {"task", snapshot.context.task}, {"phase", snapshot.context.phase}, {"completed", snapshot.context.completed},
            {"total", snapshot.context.total}, {"context_age_ms", ContextAge(snapshot.context, now)}});
        diagnostics::runtime::Event("ui_dispatch_health", {{"slow_logged", snapshot.slow_logged},
            {"slow_dropped", snapshot.slow_dropped}, {"slow_pending", snapshot.count},
            {"frame_samples", g_frame_total}, {"dispatch_samples", g_dispatch_total}, {"probe_samples", g_probe_stack_total},
            {"target_set", snapshot.window != nullptr}, {"abandoned", snapshot.abandoned}, {"rejected_acks", snapshot.rejected_acks}});
    }
}

bool AppendFrame(HANDLE file, const std::wstring& text) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || size.QuadPart + bytes > kFrameLogLimit) return false;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    return WriteFile(file, utf8.data(), static_cast<DWORD>(bytes), &written, nullptr) &&
        written == static_cast<DWORD>(bytes);
}

void RunFrameWatch(DWORD tid, HANDLE file) {
    uint64_t tracked = 0;
    size_t frame_samples = 0;
    uint64_t tracked_dispatch = 0;
    size_t dispatch_samples = 0;
    ULONGLONG last_dispatch_sample = 0;
    auto& recent = g_frame_recent;
    auto& recent_count = g_frame_recent_count;
    auto& recent_next = g_frame_recent_next;
    bool stack_logging = true;
    while (WaitForSingleObject(g_frame_stop, 12) == WAIT_TIMEOUT) {
        PollDiagnostics(); // Must precede every sampling budget/early continue.
        uint64_t probe_token = 0, probe_start = 0;
        {
            std::lock_guard lock(g_probe_mutex);
            if (stack_logging && g_probe_stack_total < 20 && g_probe.pending && !g_probe.ack_tick &&
                !g_probe.sampled && GetTickCount64() - g_probe.sent_tick >= 400 &&
                !g_frame_id.load() && !g_dispatch_id.load() && g_modal_depth.load() == 0) {
                probe_token = g_probe.pending; probe_start = g_probe.sent_tick; g_probe.sampled = true;
            }
        }
        if (probe_token) {
            ++g_probe_stack_total;
            ThreadStack stack;
            const bool ok = SampleThread(tid, stack);
            bool current = false;
            { std::lock_guard lock(g_probe_mutex); current = probe_token == g_probe.pending && !g_probe.ack_tick; }
            wchar_t header[256];
            swprintf_s(header, L"probe_token=%llu tick=%llu elapsed=%llu status=%s\r\n", probe_token,
                GetTickCount64(), GetTickCount64() - probe_start,
                !current ? L"stale_discarded" : ok ? L"captured" : L"capture_failed");
            std::wstring text = header;
            if (current && ok) for (size_t i = 0; i < stack.count; ++i) text += L"  " + FrameText(stack.frames[i], false) + L"\r\n";
            if (!AppendFrame(file, text)) stack_logging = false;
        }
        const uint64_t id = g_frame_id.load(std::memory_order_acquire);
        if (id) {
            if (!stack_logging || g_frame_total >= 100) continue;
            const uint64_t start = g_frame_start.load(std::memory_order_relaxed);
            if (id != g_frame_id.load(std::memory_order_acquire)) continue;
            const ULONGLONG now = GetTickCount64();
            if (now < start || now - start < 50) continue;
            if (tracked != id) { tracked = id; frame_samples = 0; }
            if (frame_samples >= 3 || (recent_count == 3 && now - recent[recent_next] < 1000)) continue;
            ++frame_samples;
            ++g_frame_total;
            recent[recent_next] = now;
            recent_next = (recent_next + 1) % 3;
            if (recent_count < 3) ++recent_count;
            ThreadStack stack;
            const bool ok = SampleThread(tid, stack);
            // Never attribute a sample to a completed/replaced frame, even on failure.
            const bool current = id == g_frame_id.load(std::memory_order_acquire);
            if (current && ok) MarkProbeCovered(now);
            wchar_t header[256];
            swprintf_s(header, L"frame_id=%llu tick=%llu elapsed=%llu sample=%zu status=%s\r\n",
                static_cast<unsigned long long>(id), static_cast<unsigned long long>(now),
                static_cast<unsigned long long>(now - start), frame_samples,
                !current ? L"stale_discarded" : ok ? L"captured" : L"capture_failed");
            std::wstring text = header;
            if (current && ok) {
                for (size_t i = 0; i < stack.count; ++i)
                    text += L"  " + FrameText(stack.frames[i], false) + L"\r\n";
            }
            if (!AppendFrame(file, text)) stack_logging = false;
            continue;
        }
        if (!stack_logging || g_dispatch_total >= 60 || g_modal_depth.load(std::memory_order_acquire) > 0) continue;
        const uint64_t did = g_dispatch_id.load(std::memory_order_acquire);
        if (!did) continue;
        const uint64_t start = g_dispatch_start.load(std::memory_order_relaxed);
        const uint64_t hb = g_dispatch_heartbeat.load(std::memory_order_relaxed);
        const UINT msg = g_dispatch_msg.load(std::memory_order_relaxed);
        if (did != g_dispatch_id.load(std::memory_order_acquire)) continue;
        const ULONGLONG now = GetTickCount64();
        if (now < hb || now - hb < kDispatchStallMs) continue;
        if (tracked_dispatch != did) {
            tracked_dispatch = did;
            dispatch_samples = 0;
            last_dispatch_sample = 0;
        }
        if (dispatch_samples >= 3 || (last_dispatch_sample && now - last_dispatch_sample < kDispatchResampleMs))
            continue;
        ++dispatch_samples;
        ++g_dispatch_total;
        last_dispatch_sample = now;
        ThreadStack stack;
        const bool ok = SampleThread(tid, stack);
        const bool current = did == g_dispatch_id.load(std::memory_order_acquire);
        if (current && ok) MarkProbeCovered(now);
        wchar_t header[256];
        swprintf_s(header, L"dispatch_id=%llu msg=0x%x tick=%llu elapsed=%llu sample=%zu status=%s\r\n",
            static_cast<unsigned long long>(did), static_cast<unsigned>(msg),
            static_cast<unsigned long long>(now),
            static_cast<unsigned long long>(now >= start ? now - start : 0),
            dispatch_samples,
            !current ? L"stale_discarded" : ok ? L"captured" : L"capture_failed");
        std::wstring text = header;
        if (current && ok) {
            for (size_t i = 0; i < stack.count; ++i)
                text += L"  " + FrameText(stack.frames[i], false) + L"\r\n";
        }
        if (!AppendFrame(file, text)) stack_logging = false;
    }
    PollDiagnostics(true);
    CloseHandle(file);
}

} // namespace

UINT UiProbeMessage() {
    static const UINT message = RegisterWindowMessageW(L"Pulse.Diagnostics.UiProbe.v1");
    return message;
}
void SetProbeWindow(HWND window) {
    std::lock_guard lock(g_probe_mutex);
    if (g_probe.pending && !g_probe.ack_tick) ++g_probe.abandoned;
    g_probe.window = window;
    g_probe.pending = 0; g_probe.ack_tick = 0; g_probe.next_post = 0;
}
void AckUiProbe(uint64_t token) {
    std::lock_guard lock(g_probe_mutex);
    if (token && token == g_probe.pending && !g_probe.ack_tick) {
        g_probe.ack_tick = GetTickCount64();
        ++g_probe.acked;
        g_probe.maximum = (std::max)(g_probe.maximum, g_probe.ack_tick - g_probe.sent_tick);
    } else ++g_probe.rejected_acks;
}
void SetOperationContext(uint64_t task, uint32_t phase, uint64_t completed, uint64_t total) {
    std::lock_guard lock(g_probe_mutex);
    g_probe.context = {task, phase, completed, total, GetTickCount64()};
}

void Start(HWND hwnd, const std::wstring& log_path) {
    if (g_thread.joinable() || !hwnd) return;
    g_stop = false;
    g_thread = std::thread(Run, hwnd, log_path);
}

void Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

bool StartFrameWatch(const std::wstring& log_path) {
    if (g_frame_thread.joinable()) return true;
    // A dedicated file and exclusive writer keep append size bounded.
    HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    g_frame_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_frame_stop) { CloseHandle(file); return false; }
    { std::lock_guard lock(g_probe_mutex); g_probe.last_health = GetTickCount64(); g_probe.config_due = true; }
    (void)UiProbeMessage();
    try {
        g_frame_thread = std::thread(RunFrameWatch, GetCurrentThreadId(), file);
        g_frame_enabled.store(true, std::memory_order_release);
        return true;
    } catch (...) {
        CloseHandle(file);
        CloseHandle(g_frame_stop);
        g_frame_stop = nullptr;
        return false;
    }
}

void StopFrameWatch() {
    g_frame_enabled.store(false, std::memory_order_release);
    g_frame_id.store(0, std::memory_order_release);
    g_dispatch_id.store(0, std::memory_order_release);
    g_modal_depth.store(0, std::memory_order_release);
    if (g_frame_stop) SetEvent(g_frame_stop);
    if (g_frame_thread.joinable()) g_frame_thread.join();
    if (g_frame_stop) CloseHandle(g_frame_stop);
    g_frame_stop = nullptr;
    SetProbeWindow(nullptr);
}

uint64_t BeginFrame() {
    if (!g_frame_enabled.load(std::memory_order_acquire) || g_frame_id.load() != 0) return 0;
    const uint64_t id = ++g_frame_sequence;
    g_frame_start.store(GetTickCount64(), std::memory_order_relaxed);
    g_frame_id.store(id, std::memory_order_release);
    return id;
}

void EndFrame(uint64_t id) {
    if (id) g_frame_id.compare_exchange_strong(id, 0, std::memory_order_release);
}

uint64_t BeginDispatch(UINT message) {
    if (!g_frame_enabled.load(std::memory_order_acquire)) return 0;
    if (g_dispatch_id.load() != 0) { g_dispatch_nested.store(true); return 0; }
    g_dispatch_nested.store(false);
    g_dispatch_modal.store(g_modal_depth.load() > 0);
    const uint64_t id = ++g_dispatch_sequence;
    const uint64_t now = GetTickCount64();
    g_dispatch_msg.store(message, std::memory_order_relaxed);
    g_dispatch_start.store(now, std::memory_order_relaxed);
    g_dispatch_heartbeat.store(now, std::memory_order_relaxed);
    g_dispatch_id.store(id, std::memory_order_release);
    return id;
}

void EndDispatch(uint64_t id) {
    if (!id || id != g_dispatch_id.load()) return;
    const auto now = GetTickCount64();
    const auto elapsed = now - g_dispatch_start.load();
    if (elapsed >= 100) {
        std::lock_guard lock(g_probe_mutex);
        if (g_probe.count == g_probe.slow.size()) ++g_probe.slow_dropped;
        else {
            const auto index = (g_probe.head + g_probe.count++) % g_probe.slow.size();
            g_probe.slow[index] = {id, g_dispatch_msg.load(), elapsed, g_dispatch_modal.load(),
                g_dispatch_nested.load(), now, g_probe.context};
        }
    }
    g_dispatch_id.compare_exchange_strong(id, 0, std::memory_order_release);
}

void NoteDispatchHeartbeat() {
    if (g_dispatch_id.load(std::memory_order_relaxed) != 0)
        g_dispatch_heartbeat.store(GetTickCount64(), std::memory_order_relaxed);
}

void PushModalPause() {
    if (g_dispatch_id.load()) g_dispatch_modal.store(true);
    g_modal_depth.fetch_add(1, std::memory_order_acq_rel);
}

void PopModalPause() {
    int current = g_modal_depth.load(std::memory_order_acquire);
    while (current > 0 && !g_modal_depth.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel)) {
    }
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
