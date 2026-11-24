#include "app/hang_watch.h"
#include "common/runtime_log.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
std::string Read(const std::wstring& path) {
    std::ifstream file{std::filesystem::path(path)};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
    return count;
}
}

namespace {
constexpr UINT kBlockProbe = WM_APP + 113;
unsigned probe_acks = 0;
LRESULT CALLBACK ProbeProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
#ifdef PULSE_UI_PROBE_API
    if (message == pulse::app::hang::UiProbeMessage()) {
        ++probe_acks;
        pulse::app::hang::AckUiProbe(static_cast<uint64_t>(wp));
        return 0;
    }
#endif
    if (message == kBlockProbe) {
        const auto dispatch = pulse::app::hang::BeginDispatch(message);
        Sleep(static_cast<DWORD>(wp));
        pulse::app::hang::EndDispatch(dispatch);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
void PumpFor(DWORD ms) {
    const auto until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(1);
    }
}
int ProbeTest() {
    using namespace pulse::app::hang;
    namespace runtime = pulse::diagnostics::runtime;
    const auto root = std::filesystem::absolute(std::filesystem::path("bench_data/copy-diagnostics-20261120") /
        ("probe-" + std::to_string(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    runtime::Initialize(root.wstring(), "probe-test");
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseProbePrivate"; wc.lpfnWndProc = ProbeProc;
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    const auto frame_log = (root / "frame.log").wstring();
    StartFrameWatch(frame_log);
#ifdef PULSE_UI_PROBE_API
    SetOperationContext(704, 3, 10, 100);
    SetProbeWindow(window);
#endif
    SendMessageW(window, kBlockProbe, 200, 0);
    PumpFor(120);
#ifdef PULSE_UI_PROBE_API
    SetProbeWindow(nullptr); SetProbeWindow(window);
#endif
    SendMessageW(window, kBlockProbe, 650, 0);
    PumpFor(5300);
    unsigned no_target_before = 0, stopped_before = 0, restarted_before = 0;
    bool no_target_ok = false, stopped_ok = false, restart_ok = false;
#ifdef PULSE_UI_PROBE_API
    SetProbeWindow(nullptr);
    PumpFor(100); no_target_before = probe_acks;
    PumpFor(600); no_target_ok = probe_acks == no_target_before;
#endif
    StopFrameWatch();
    PumpFor(100); stopped_before = probe_acks;
    PumpFor(600); stopped_ok = probe_acks == stopped_before;
    StartFrameWatch(frame_log);
#ifdef PULSE_UI_PROBE_API
    restarted_before = probe_acks;
    SetProbeWindow(window); PumpFor(600); restart_ok = probe_acks > restarted_before;
    SetProbeWindow(nullptr); SetProbeWindow(window);
    Sleep(650); // Real UI stall outside BeginDispatch/GetMessage instrumentation.
    PumpFor(150);
    AckUiProbe(UINT64_MAX); // Stale/nonmatching acknowledgements never complete a pending probe.
    SetProbeWindow(nullptr);
    DestroyWindow(window);
    SetProbeWindow(window); // Deliberately stale HWND: genuine PostMessage failure.
    PumpFor(1100);
    SetProbeWindow(nullptr);
#endif
    StopFrameWatch();
    runtime::Flush(); runtime::Shutdown();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::string logs;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root))
        if (item.path().extension() == ".jsonl") logs += Read(item.path().wstring());
    int failed = 0;
    const auto check = [&](bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); failed += !ok; };
    check(logs.find("ui_probe_config") != std::string::npos && logs.find("\"period_ms\":500") != std::string::npos, "runtime config identifies registered probe message and thresholds");
    check(Count(logs, "ui_probe_health") >= 1 && Count(logs, "ui_probe_health") <= 5, "real idle message pump produces bounded health evidence");
    check(Count(logs, "ui_probe_late") >= 2 && Count(logs, "ui_probe_late") <= 5, "200ms and 650ms real handlers generate bounded late evidence");
    check(Count(logs, "ui_probe_recovered") >= 2, "actual posted nonce acknowledgement records recovery");
    check(Count(logs, "ui_slow_dispatch") >= 2, "both blocked message handlers record slow dispatch after return");
    check(logs.find("\"task\":704") != std::string::npos, "probe evidence carries operation task context");
    check(no_target_ok && stopped_ok, "no-target and stopped monitor do not keep posting");
    check(restart_ok, "restart resumes real probes with monotonic tokens");
    check(Count(Read(frame_log), "probe_token=") == 1, "untracked 650ms block gets one probe stack without duplicating dispatch coverage");
    check(logs.find("\"post_error\":1400") != std::string::npos, "destroyed HWND yields bounded actual PostMessage failure evidence");
    check(logs.find("\"rejected_acks\":1") != std::string::npos, "stale nonce acknowledgement is counted but ignored");
    check(logs.find("\"context_age_ms\":") != std::string::npos, "operation context explicitly records observation age");
    std::printf("RESULT failures=%d acks=%u fixture=%ls\n", failed, probe_acks, root.c_str());
    return failed ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--ui-probe-only") return ProbeTest();
    if (argc != 1) return 2;
    using namespace pulse::app::hang;
    const auto folder = std::filesystem::absolute("bench_data/copy-diagnostics-20261120");
    std::filesystem::create_directories(folder);
    const std::wstring temp = folder.wstring() + L"\\";
    const std::wstring path = temp + L"pulse_frame_watch_" +
        std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(path.c_str());
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    check(!StartFrameWatch(std::wstring(temp)) && BeginFrame() == 0, "invalid log target reports unavailable");
    check(StartFrameWatch(path), "monitor starts with writable log");
    for (int i = 0; i < 20; ++i) {
        const auto id = BeginFrame();
        Sleep(2);
        EndFrame(id);
    }
    Sleep(30);
    check(Read(path).empty(), "short frames do not sample");
    const auto slow = BeginFrame();
    check(slow != 0 && BeginFrame() == 0, "nested frame preserves outer identity");
    Sleep(120);
    EndFrame(slow);
    Sleep(30);
    const auto text = Read(path);
    check(text.find("frame_id=" + std::to_string(slow) + " ") != std::string::npos &&
        text.find("status=captured") != std::string::npos && text.find("+0x") != std::string::npos,
        "120ms stall captures correlated module RVA stack");
    check(Count(text, "frame_id=") >= 1 && Count(text, "frame_id=") <= 3,
        "per frame sampling bounded at three");
    const auto second = BeginFrame();
    Sleep(120);
    EndFrame(second);
    Sleep(30);
    check(Count(Read(path), "frame_id=") <= 3, "rolling second sampling bounded at three");
    {
        // UI message-handler stall capture (>= 400ms), heartbeat suppression, and modal pause.
        const auto hb = BeginDispatch(WM_TIMER);
        for (int i = 0; i < 6; ++i) {
            Sleep(90);
            NoteDispatchHeartbeat();
        }
        EndDispatch(hb);
        Sleep(30);
        check(Read(path).find("dispatch_id=" + std::to_string(hb) + " ") == std::string::npos,
              "active heartbeats suppress dispatch stall capture during modal loops");

        PushModalPause();
        const auto modal = BeginDispatch(WM_SYSCOMMAND);
        Sleep(460);
        EndDispatch(modal);
        PopModalPause();
        Sleep(30);
        check(Read(path).find("dispatch_id=" + std::to_string(modal) + " ") == std::string::npos,
              "modal pause suppresses dispatch stall capture");

        const auto blocked = BeginDispatch(WM_RBUTTONUP);
        check(blocked != 0 && BeginDispatch(WM_KEYDOWN) == 0, "nested dispatch preserves outer identity");
        Sleep(460);
        EndDispatch(blocked);
        Sleep(30);
        const auto dtext = Read(path);
        check(dtext.find("dispatch_id=" + std::to_string(blocked) + " msg=0x205 ") != std::string::npos &&
              dtext.find("status=captured") != std::string::npos,
              "460ms UI message stall captures correlated module RVA stack");
    }
    const auto before_stop = GetTickCount64();
    StopFrameWatch();
    check(GetTickCount64() - before_stop < 500 && BeginFrame() == 0, "stop joins promptly and disables sampling");
    StopFrameWatch();
    StartFrameWatch(path);
    const auto restart = BeginFrame();
    StopFrameWatch();
    EndFrame(restart);
    check(restart > second, "restart preserves unique frame IDs");
    DeleteFileW(path.c_str());
    return failures ? 1 : 0;
}
