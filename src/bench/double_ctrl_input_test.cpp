#ifdef DOUBLE_CTRL_BASELINE
#include "../original/src/app/double_ctrl.h"
#else
#include "../app/double_ctrl.h"
#endif
#include <cstdio>
#include <vector>
#include <algorithm>
#include <array>
#include <barrier>
#include <cstring>
#include <map>
#include <string>
#include <winternl.h>

#ifndef DOUBLE_CTRL_BASELINE
namespace pulse {
struct DoubleCtrlWatcherTestAccess {
    static ULONG_PTR Generation(const DoubleCtrlWatcher& watcher) { return watcher.generation_.load(); }
    static bool Released(const DoubleCtrlWatcher& watcher) {
        return !watcher.thread_.joinable() && !watcher.ready_ && !watcher.stop_;
    }
    static std::array<HANDLE, 3> Owned(DoubleCtrlWatcher& watcher) {
        return {watcher.ready_, watcher.stop_, watcher.thread_.joinable() ? watcher.thread_.native_handle() : nullptr};
    }
    static void Key(DoubleCtrlWatcher& watcher, UINT key, bool down, DWORD flags = 0) {
        KBDLLHOOKSTRUCT event{};
        event.vkCode = key;
        event.flags = flags;
        DoubleCtrlWatcher::current_ = &watcher;
        DoubleCtrlWatcher::KeyboardProc(HC_ACTION, down ? WM_KEYDOWN : WM_KEYUP,
            reinterpret_cast<LPARAM>(&event));
        DoubleCtrlWatcher::current_ = nullptr;
    }
    static void Mouse(DoubleCtrlWatcher& watcher, UINT message, DWORD flags = 0) {
        MSLLHOOKSTRUCT event{};
        event.flags = flags;
        DoubleCtrlWatcher::current_ = &watcher;
        DoubleCtrlWatcher::MouseProc(HC_ACTION, message, reinterpret_cast<LPARAM>(&event));
        DoubleCtrlWatcher::current_ = nullptr;
    }
};
}
#endif

namespace {
int failures = 0;
void Check(bool result, const char* name) {
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
    if (!result) ++failures;
}
#ifndef DOUBLE_CTRL_BASELINE
// Diagnostic-only native APIs: retain concrete handle/type/ownership evidence
// if the process-wide count fails. Product code never uses these interfaces.
struct NativeHandleEntry {
    HANDLE handle;
    ULONG_PTR handle_count, pointer_count;
    ACCESS_MASK access;
    ULONG type_index, attributes, reserved;
};
struct NativeHandleList {
    ULONG_PTR count, reserved;
    NativeHandleEntry handles[1];
};
struct HandleInfo {
    std::wstring type;
    ULONG access = 0;
    ULONG type_index = 0;
};
using HandleSnapshot = std::map<ULONG_PTR, HandleInfo>;
using HandleOwners = std::map<ULONG_PTR, std::string>;
HandleSnapshot Snapshot() {
    using QueryProcess = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using QueryObject = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto process = reinterpret_cast<QueryProcess>(GetProcAddress(module, "NtQueryInformationProcess"));
    const auto object = reinterpret_cast<QueryObject>(GetProcAddress(module, "NtQueryObject"));
    HandleSnapshot result;
    if (!process || !object) { Check(false, "native handle diagnostics available"); return result; }
    std::vector<unsigned char> buffer(65536);
    ULONG required = 0;
    NTSTATUS status = process(GetCurrentProcess(), 51, buffer.data(), static_cast<ULONG>(buffer.size()), &required);
    if (status < 0 && required > buffer.size()) {
        buffer.resize(required);
        status = process(GetCurrentProcess(), 51, buffer.data(), static_cast<ULONG>(buffer.size()), &required);
    }
    if (status < 0) {
        std::printf("HANDLE_SNAPSHOT_ERROR status=0x%lx required=%lu\n", status, required);
        Check(false, "capture native handle snapshot");
        return result;
    }
    const auto* list = reinterpret_cast<const NativeHandleList*>(buffer.data());
    if (list->count > (buffer.size() - offsetof(NativeHandleList, handles)) / sizeof(NativeHandleEntry)) {
        Check(false, "native handle snapshot bounds"); return result;
    }
    for (ULONG_PTR i = 0; i < list->count; ++i) {
        const auto& entry = list->handles[i];
        std::array<unsigned char, 4096> info{};
        status = object(entry.handle, 2, info.data(), static_cast<ULONG>(info.size()), &required);
        const auto* type = reinterpret_cast<const UNICODE_STRING*>(info.data());
        result.emplace(reinterpret_cast<ULONG_PTR>(entry.handle), HandleInfo{
            status >= 0 ? std::wstring(type->Buffer, type->Length / sizeof(wchar_t)) : L"closed-during-snapshot",
            entry.access, entry.type_index});
    }
    return result;
}
void ReportHandles(const char* stage, const HandleSnapshot& before, const HandleSnapshot& after,
        const HandleOwners& owners) {
    std::map<std::wstring, std::pair<size_t, size_t>> types;
    for (const auto& [handle, info] : before) ++types[info.type].first;
    for (const auto& [handle, info] : after) ++types[info.type].second;
    std::printf("HANDLE_SNAPSHOT stage=%s before=%zu after=%zu\n", stage, before.size(), after.size());
    for (const auto& [type, counts] : types)
        std::printf("HANDLE_TYPE stage=%s type=%ls before=%zu after=%zu\n", stage, type.c_str(), counts.first, counts.second);
    for (const auto& [handle, info] : after) {
        const auto old = before.find(handle);
        if (old != before.end() && old->second.type_index == info.type_index && old->second.access == info.access) continue;
        const auto owner = owners.find(handle);
        std::printf("HANDLE_NEW stage=%s handle=0x%llx type=%ls access=0x%lx last_owner=%s\n", stage,
            static_cast<unsigned long long>(handle), info.type.c_str(), info.access,
            owner == owners.end() ? "not-created-by-watcher" : owner->second.c_str());
    }
}
bool OwnedClosed(const std::array<HANDLE, 3>& handles, HandleOwners& owners, int cycle) {
    constexpr const char* names[] = {"ready-event", "stop-event", "listener-thread"};
    bool closed = true;
    for (size_t i = 0; i < handles.size(); ++i) {
        if (!handles[i]) { closed = false; continue; }
        owners[reinterpret_cast<ULONG_PTR>(handles[i])] = std::string(names[i]) + " cycle=" + std::to_string(cycle);
        DWORD flags = 0;
        SetLastError(ERROR_SUCCESS);
        const BOOL valid = GetHandleInformation(handles[i], &flags);
        const DWORD error = GetLastError();
        if (valid || error != ERROR_INVALID_HANDLE) {
            closed = false;
            std::printf("OWNED_HANDLE_REMAINS cycle=%d owner=%s handle=%p valid=%d error=%lu\n",
                cycle, names[i], handles[i], valid, error);
        }
    }
    return closed;
}
#endif
bool Tap(pulse::DoubleCtrlDetector& d, uint64_t at) {
    d.Key(true, true, at);
    return d.Key(true, false, at + 40);
}
void Common() {
    pulse::DoubleCtrlDetector d;
    Check(!Tap(d, 0) && Tap(d, 100), "same-side double tap");
    d = {};
    Check(!Tap(d, 0) && Tap(d, 100) && !Tap(d, 200), "three taps fire once");
    d = {};
    Check(!Tap(d, 0) && Tap(d, 100) && !Tap(d, 200) && Tap(d, 300), "four taps fire twice without cooldown");
    d = {};
    d.Key(true, true, 0); d.Key(true, true, 10); d.Key(true, false, 40);
    d.Key(true, false, 50);
    Check(Tap(d, 100), "duplicate down and up do not add taps");
    d = {};
    d.Key(true, true, 0); d.Key(true, false, 281);
    Check(!Tap(d, 300), "long hold rejected");
    d = {};
    Tap(d, 0);
    Check(!Tap(d, 341), "gap boundary rejects 301ms");
    d = {};
    Tap(d, 0);
    Check(Tap(d, 340), "gap boundary accepts 300ms");
    d = {};
    Tap(d, 0); d.Interrupt();
    Check(!Tap(d, 100), "wheel or complete click between taps rejected");
    d = {};
    d.Key(false, true, 0);
    Check(!Tap(d, 20) && !Tap(d, 100), "previously held non-Ctrl key blocks gesture");
}
#ifndef DOUBLE_CTRL_BASELINE
void Physical() {
    using pulse::DoubleCtrlDetector;
    DoubleCtrlDetector d;
    auto tap = [&](UINT key, uint64_t t) {
        d.PhysicalKey(key, true, t);
        return d.PhysicalKey(key, false, t + 40);
    };
    Check(!tap(VK_LCONTROL, 0) && !tap(VK_RCONTROL, 100), "alternating Ctrl sides do not form a pair");
    Check(tap(VK_RCONTROL, 200), "different side starts a fresh first tap");
    d = {};
    d.PhysicalKey(VK_LCONTROL, true, 0);
    d.PhysicalKey(VK_RCONTROL, true, 10);
    d.PhysicalKey(VK_LCONTROL, false, 20);
    d.PhysicalKey(VK_RCONTROL, false, 30);
    Check(!tap(VK_LCONTROL, 100), "overlapping Ctrl sides cannot form a tap");
    d = {};
    d.Prime(VK_LSHIFT);
    Check(!tap(VK_LCONTROL, 0) && !tap(VK_LCONTROL, 100), "startup held Shift blocks taps");
    d.PhysicalKey(VK_LSHIFT, false, 160);
    Check(!tap(VK_LCONTROL, 200) && tap(VK_LCONTROL, 300), "released startup modifier permits fresh gesture");
    d = {};
    d.Prime(VK_LCONTROL);
    d.PhysicalKey(VK_LCONTROL, false, 20);
    Check(!tap(VK_LCONTROL, 100), "startup held Ctrl release is not a tap");
    d = {};
    d.Prime(VK_RBUTTON);
    Check(!tap(VK_LCONTROL, 0) && !tap(VK_LCONTROL, 100), "startup held mouse button blocks taps");
    d = {};
    tap(VK_LCONTROL, 0);
    d.PhysicalKey(VK_LBUTTON, true, 50);
    d.PhysicalKey(VK_LBUTTON, false, 60);
    Check(!tap(VK_LCONTROL, 100), "complete intervening click blocks gesture");
    d = {};
    d.PhysicalKey(VK_LCONTROL, true, 0, true);
    d.PhysicalKey(VK_LCONTROL, false, 40, true);
    d.PhysicalKey(VK_LCONTROL, true, 100, true);
    Check(!d.PhysicalKey(VK_LCONTROL, false, 140, true), "injected Ctrl taps never fire");
    d = {};
    tap(VK_LCONTROL, 0);
    d.PhysicalKey('C', true, 50, true);
    d.PhysicalKey('C', false, 60, true);
    Check(!tap(VK_LCONTROL, 100), "injected combination between physical taps spoils gesture");
    d = {};
    d.PhysicalKey(VK_LSHIFT, true, 0, true);
    Check(!tap(VK_LCONTROL, 20) && !tap(VK_LCONTROL, 100), "held injected modifier blocks physical taps");
    d.PhysicalKey(VK_LSHIFT, false, 160, true);
    Check(!tap(VK_LCONTROL, 200) && tap(VK_LCONTROL, 300), "fresh physical pair recovers after injected modifier release");
    d = {};
    d.PhysicalKey(VK_LCONTROL, true, 0);
    d.PhysicalKey(VK_RCONTROL, false, 10);
    d.PhysicalKey(VK_LCONTROL, false, 40);
    Check(tap(VK_LCONTROL, 100), "unmatched opposite-side release cannot complete or cancel tap");
}
void Callbacks() {
    using Access = pulse::DoubleCtrlWatcherTestAccess;
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    auto count = [&] {
        int result = 0;
        while (PeekMessageW(&msg, nullptr, WM_APP + 1, WM_APP + 1, PM_REMOVE)) ++result;
        return result;
    };
    auto tap = [](pulse::DoubleCtrlWatcher& watcher, DWORD flags = 0) {
        Access::Key(watcher, VK_CONTROL, true, flags);
        Access::Key(watcher, VK_CONTROL, false, flags);
    };
    {
        pulse::DoubleCtrlWatcher watcher;
        tap(watcher); tap(watcher, LLKHF_EXTENDED);
        Check(count() == 0, "production keyboard callback maps extended Ctrl to distinct side");
        tap(watcher, LLKHF_EXTENDED);
        Check(count() == 1, "production keyboard callback posts one right-side pair trigger");
    }
    {
        pulse::DoubleCtrlWatcher watcher;
        tap(watcher, LLKHF_INJECTED); tap(watcher, LLKHF_LOWER_IL_INJECTED);
        Check(count() == 0, "production callback ignores both injection flags");
    }
    for (const UINT message : {WM_LBUTTONDOWN, WM_MOUSEWHEEL, WM_MOUSEHWHEEL}) {
        pulse::DoubleCtrlWatcher watcher;
        tap(watcher);
        Access::Mouse(watcher, message);
        if (message == WM_LBUTTONDOWN) Access::Mouse(watcher, WM_LBUTTONUP);
        tap(watcher);
        Check(count() == 0, "production mouse callback spoils click or wheel between taps");
    }
    {
        pulse::DoubleCtrlWatcher watcher;
        Access::Mouse(watcher, WM_LBUTTONDOWN);
        Access::Mouse(watcher, WM_LBUTTONUP, LLMHF_INJECTED);
        tap(watcher); tap(watcher);
        Check(count() == 0, "injected mouse release cannot clear physical held button");
    }
    {
        pulse::DoubleCtrlWatcher watcher;
        tap(watcher);
        Access::Mouse(watcher, WM_MOUSEMOVE);
        tap(watcher);
        Check(count() == 1, "mouse movement does not spoil clean taps");
    }
}
std::vector<RAWINPUTDEVICE> Registrations() {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) {
        Check(false, "read raw registrations size"); return {};
    }
    std::vector<RAWINPUTDEVICE> result(count);
    if (count && GetRegisteredRawInputDevices(result.data(), &count, sizeof(RAWINPUTDEVICE)) == UINT(-1))
        Check(false, "read raw registrations");
    return result;
}
bool Same(const std::vector<RAWINPUTDEVICE>& a, const std::vector<RAWINPUTDEVICE>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].usUsagePage != b[i].usUsagePage || a[i].usUsage != b[i].usUsage ||
            a[i].dwFlags != b[i].dwFlags || a[i].hwndTarget != b[i].hwndTarget) return false;
    return true;
}
void Lifecycle(bool leak_fixture) {
    using Access = pulse::DoubleCtrlWatcherTestAccess;
    const HWND target = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(target != nullptr, "isolated message-only target created without focus");
    if (!target) return;
    RAWINPUTDEVICE sentinels[] = {{1, 6, RIDEV_INPUTSINK, target}, {1, 2, RIDEV_INPUTSINK, target}};
    Check(RegisterRawInputDevices(sentinels, 2, sizeof(RAWINPUTDEVICE)) != FALSE,
        "test-only sentinel raw keyboard and mouse owners registered");
    const auto before = Registrations();
    pulse::DoubleCtrlWatcher watcher;
    HandleOwners owners;
    const auto startup_before = Snapshot();
    watcher.Update(target, WM_APP + 20, false, 300);
    Check(!watcher.Running() && Access::Released(watcher), "disabled starts no thread or hooks");
    watcher.Update(target, WM_APP + 20, true, 300);
    Check(watcher.Running() && watcher.LastError() == 0, "real keyboard and mouse hooks installed");
    const auto generation = Access::Generation(watcher);
    Check(watcher.IsCurrentTrigger(generation) && !watcher.IsCurrentTrigger(0), "current generation validated");
    Check(Same(before, Registrations()), "enabled watcher does not claim raw registrations");
    watcher.Update(target, WM_APP + 20, true, 300);
    Check(Access::Generation(watcher) == generation, "identical update does not restart listener");
    PostMessageW(target, WM_APP + 20, generation, 0);
    watcher.Update(target, WM_APP + 20, true, 400);
    MSG msg{};
    const bool queued = PeekMessageW(&msg, target, WM_APP + 20, WM_APP + 20, PM_REMOVE) != FALSE;
    Check(queued && !watcher.IsCurrentTrigger(msg.wParam), "configuration change rejects actual queued old trigger");
    const auto startup_owned = Access::Owned(watcher);
    watcher.Stop();
    Check(!watcher.Running() && Access::Released(watcher), "stop joins and releases synchronization handles");
    Check(OwnedClosed(startup_owned, owners, 0), "stopped ready/stop/thread handles are invalid in the OS");
    const auto startup_after = Snapshot();
    ReportHandles("first-start-stop", startup_before, startup_after, owners);
    const auto snapshot_before = Snapshot();
    DWORD handles_before = 0, handles_after = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &handles_before) != FALSE, "read baseline process handle count");
    std::array<DWORD, 32> handle_counts{};
    bool cycles = true;
    bool owned_closed = true;
    bool counts_read = true;
    HANDLE fixture_handle = nullptr;
    for (int i = 0; i < 32; ++i) {
        watcher.Update(target, WM_APP + 20, true, 300);
        cycles = cycles && watcher.Running();
        const auto owned = Access::Owned(watcher);
        watcher.Stop();
        cycles = cycles && !watcher.Running() && Access::Released(watcher);
        owned_closed = OwnedClosed(owned, owners, i + 1) && owned_closed;
        if (leak_fixture && i == 15) {
            fixture_handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            owners[reinterpret_cast<ULONG_PTR>(fixture_handle)] = "intentional-negative-control-event";
        }
        counts_read = (GetProcessHandleCount(GetCurrentProcess(), &handle_counts[i]) != FALSE) && counts_read;
        if (handle_counts[i] > handles_before && (i == 0 || handle_counts[i] != handle_counts[i - 1])) {
            const auto stage = "growth-cycle-" + std::to_string(i + 1);
            ReportHandles(stage.c_str(), snapshot_before, Snapshot(), owners);
        }
    }
    counts_read = (GetProcessHandleCount(GetCurrentProcess(), &handles_after) != FALSE) && counts_read;
    std::printf("HANDLE_COUNT before=%lu after=%lu cycles=", handles_before, handles_after);
    for (const auto handles : handle_counts) std::printf("%lu,", handles);
    std::printf("\n");
    Check(cycles, "32 real hook install and immediate stop cycles");
    Check(owned_closed, "all 96 watcher-owned kernel handles close at their Stop boundary");
    Check(counts_read, "all process handle counts obtained successfully");
    ReportHandles("repeated-stop", snapshot_before, Snapshot(), owners);
    Check(handles_after <= handles_before, "repeated cycles leak no process handles");
    if (fixture_handle) CloseHandle(fixture_handle);
    const auto concurrent_before = Snapshot();
    DWORD concurrent_count_before = 0, concurrent_count_after = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &concurrent_count_before) != FALSE,
        "read concurrent baseline process handle count");
    std::barrier start(3);
    std::thread update([&] {
        start.arrive_and_wait();
        for (int i = 0; i < 16; ++i) watcher.Update(target, WM_APP + 20, true, i % 2 ? 300 : 400);
    });
    std::thread stop([&] {
        start.arrive_and_wait();
        for (int i = 0; i < 16; ++i) watcher.Stop();
    });
    start.arrive_and_wait();
    update.join();
    stop.join();
    watcher.Stop();
    Check(GetProcessHandleCount(GetCurrentProcess(), &concurrent_count_after) != FALSE,
        "read concurrent final process handle count");
    Check(!watcher.Running() && Access::Released(watcher), "concurrent Update/Stop serialize and release all state");
    ReportHandles("concurrent-stop", concurrent_before, Snapshot(), owners);
    Check(concurrent_count_after <= concurrent_count_before, "concurrent lifecycle leaks no process handles");
    Check(Same(before, Registrations()), "stop preserves raw registrations");
    watcher.Update(nullptr, WM_APP + 20, true, 300);
    Check(!watcher.Running(), "missing target does not start hook");
    for (auto& device : sentinels) { device.dwFlags = RIDEV_REMOVE; device.hwndTarget = nullptr; }
    Check(RegisterRawInputDevices(sentinels, 2, sizeof(RAWINPUTDEVICE)) != FALSE,
        "test-only sentinel registrations removed");
    DestroyWindow(target);
}
#endif
}
int main(int argc, char** argv) {
    Common();
#ifndef DOUBLE_CTRL_BASELINE
    Physical();
    Callbacks();
    Lifecycle(argc == 2 && std::strcmp(argv[1], "--diagnostic-leak-fixture") == 0);
#endif
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
