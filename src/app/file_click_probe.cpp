#include "app_internal.h"
#include "file_click_controller.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <cstring>
#include <commctrl.h>

namespace {
std::string Utf8(const std::wstring& text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
struct ClickTrace {
    pulse::AppState& state;
    FILE* log;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& trace = *reinterpret_cast<ClickTrace*>(data);
        const bool tracked = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
            message == WM_CAPTURECHANGED || message == WM_ACTIVATE || message == WM_KILLFOCUS;
        const auto emit = [&](const char* phase) {
            const auto* tab = pulse::ActiveTab(trace.state);
            fprintf(trace.log, "[MESSAGE] %s msg=%u time=%lu path=%s generation=%llu row=%d pressed=%d pending_rename=%d rename_index=%d\n",
                phase, message, static_cast<unsigned long>(GetMessageTime()), tab ? Utf8(tab->current_path).c_str() : "<no tab>",
                static_cast<unsigned long long>(tab ? tab->view_generation : 0), trace.state.fileClick.Item().index,
                trace.state.fileClick.Pressed(), trace.state.fileClick.RenamePending(), trace.state.renameIndex);
            fflush(trace.log);
        };
        if (tracked) emit("before");
        const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
        if (tracked) emit("after");
        return result;
    }
    ~ClickTrace() { RemoveWindowSubclass(state.hwnd, Proc, 2); }
};
}

int RunFileClickProbe(pulse::AppState& s, FILE* log) {
    using namespace pulse;
    wchar_t data[32768]{};
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) return 2;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return 2;
    ClickTrace message_trace{s, log};
    if (!SetWindowSubclass(s.hwnd, ClickTrace::Proc, 2, reinterpret_cast<DWORD_PTR>(&message_trace))) return 2;
    struct PointerCleanup {
        HWND hwnd;
        POINT original{};
        bool valid = false;
        bool pressed = false;
        ~PointerCleanup() {
            if (pressed) {
                INPUT input[3]{};
                input[0].type = input[1].type = INPUT_KEYBOARD;
                input[0].ki.wVk = input[1].ki.wVk = VK_ESCAPE;
                input[1].ki.dwFlags = KEYEVENTF_KEYUP;
                input[2].type = INPUT_MOUSE; input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(3, input, sizeof(INPUT));
            }
            if (GetCapture() == hwnd) ReleaseCapture();
            if (valid) SetCursorPos(original.x, original.y);
        }
    } pointer{s.hwnd};
    pointer.valid = GetCursorPos(&pointer.original) != FALSE;
    const std::wstring root = fs::NormalizePath(std::wstring(data) + L"\\file-click-fixture");
    if (root.empty()) return 2;
    const std::wstring folder = root + L"\\alpha-folder";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW((folder + L"\\nested-folder").c_str(), nullptr);
    CreateDirectoryW((root + L"\\bravo-folder").c_str(), nullptr);
    const auto pump = [](ULONGLONG duration) {
        const auto end = GetTickCount64() + duration;
        MSG message{};
        while (GetTickCount64() < end) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(1);
        }
    };
    int failures = 0;
    const auto check = [&](bool pass, const char* text) {
        fprintf(log, "[%s] %s\n", pass ? "PASS" : "FAIL", text); fflush(log);
        if (!pass) ++failures;
        return pass;
    };
    const auto trace_path = [&](const char* stage, const std::wstring& expected) {
        const auto* tab = ActiveTab(s);
        fprintf(log, "[STATE] stage=%s actual=%s expected=%s rename_index=%d loading=%d generation=%llu\n",
            stage, tab ? Utf8(tab->current_path).c_str() : "<no tab>", Utf8(expected).c_str(), s.renameIndex,
            tab ? tab->loading : false, static_cast<unsigned long long>(tab ? tab->view_generation : 0));
        fflush(log);
    };
    s.shot.active = false;
    s.appPrefs.single_click_open = false;
    s.appPrefs.hover_select = false;
    ShowWindow(s.hwnd, SW_RESTORE);
    if (!IsWindowVisible(s.hwnd)) ShowWindow(s.hwnd, SW_SHOWNORMAL);
    SetLastError(ERROR_SUCCESS);
    SetForegroundWindow(s.hwnd);
    pump(100);
    bool attached = false;
    if (IsWindowVisible(s.hwnd) && GetForegroundWindow() != s.hwnd) {
        const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        if (foreground_thread && foreground_thread != GetCurrentThreadId()) {
            attached = AttachThreadInput(GetCurrentThreadId(), foreground_thread, TRUE) != FALSE;
            if (attached) {
                SetForegroundWindow(s.hwnd);
                AttachThreadInput(GetCurrentThreadId(), foreground_thread, FALSE);
                pump(100);
            }
        }
    }
    fprintf(log, "[STATE] initial visible=%d iconic=%d foreground=%p pulse=%p current_thread=%lu foreground_thread=%lu attached=%d error=%lu\n",
        IsWindowVisible(s.hwnd), IsIconic(s.hwnd), static_cast<void*>(GetForegroundWindow()), static_cast<void*>(s.hwnd),
        GetCurrentThreadId(), GetWindowThreadProcessId(GetForegroundWindow(), nullptr), attached, GetLastError());
    fflush(log);
    if (!check(IsWindowVisible(s.hwnd) && GetForegroundWindow() == s.hwnd,
        "isolated Pulse is foreground before click probe")) return 2;
    const auto home = [&] {
        if (s.renameIndex >= 0) HideRenameOverlay(s, false);
        NavigateTo(s, root);
        const auto end = GetTickCount64() + 5000;
        do {
            pump(30);
            const auto* tab = ActiveTab(s);
            if (tab && !tab->loading && tab->EntryCount() == 2) return true;
        } while (GetTickCount64() < end);
        return false;
    };
    const auto locate = [&](POINT& point, int& row) {
        auto* tab = ActiveTab(s);
        tab->view_mode = ui::ViewMode::Details;
        tab->scroll_y = 0;
        const auto vm = BuildVm(s, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        for (int y = 100; y < static_cast<int>(bounds.bottom) - 50; y += 3) {
            for (int x = 100; x < static_cast<int>(bounds.right) - 100; x += 3) {
                const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
                if (hit.region != ui::HitTestResult::Row || hit.index < 0 ||
                    tab->EntryAt(hit.index).name != L"alpha-folder") continue;
                bool interior = true;
                for (int dy : {-4, 0, 4})
                    for (int dx : {-4, 0, 4})
                        interior = interior && PointInHitItemName(s, vm, hit, static_cast<float>(x + dx), static_cast<float>(y + dy));
                if (interior) {
                    point = {x, y}; row = hit.index; return true;
                }
            }
        }
        return false;
    };
    POINT point{};
    int row = -1;
    const auto reset = [&] {
        if (!home() || !locate(point, row)) return false;
        ActiveTab(s)->SelectOnly(row);
        s.appPrefs.single_click_open = false;
        s.activateClickTick = 0;
        return true;
    };
    const auto event = [&](UINT message, WPARAM flags = 0) {
        PostMessageW(s.hwnd, message, flags, message == WM_KEYDOWN ? 1 : MAKELPARAM(point.x, point.y));
        pump(25);
    };
    const auto click = [&] { event(WM_LBUTTONDOWN, MK_LBUTTON); event(WM_LBUTTONUP); };
    const auto interval = static_cast<ULONGLONG>(GetDoubleClickTime());
    const auto physical = [&](DWORD flags) {
        POINT screen = point;
        ClientToScreen(s.hwnd, &screen);
        HWND hit = WindowFromPoint(screen);
        if (!hit || GetAncestor(hit, GA_ROOT) != s.hwnd || GetForegroundWindow() != s.hwnd) {
            check(false, "physical input stopped because isolated Pulse no longer owns the pointer");
            return false;
        }
        INPUT input{}; input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(screen.x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
        input.mi.dy = MulDiv(screen.y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
        input.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        if (!check(SendInput(1, &input, sizeof(INPUT)) == 1, "physical file click input delivered")) return false;
        pointer.pressed = (flags & MOUSEEVENTF_LEFTDOWN) != 0;
        pump(15);
        return true;
    };
    const auto physical_click = [&] { return physical(MOUSEEVENTF_LEFTDOWN) && physical(MOUSEEVENTF_LEFTUP); };
    if (!reset()) return 3;
    pump(interval + 100);
    if (!physical_click()) return 4;
    check(s.fileClick.RenamePending(), "selected name click arms slow rename");
    if (!physical_click()) return 4;
    pump(interval + 100);
    trace_path("physical-double", folder);
    check(ActiveTab(s)->current_path == folder && s.renameIndex < 0,
        "Windows-generated selected file double click opens folder without rename overlay");
    if (!reset()) return 3;
    pump(interval + 100);
    if (!physical_click()) return 4;
    pump(interval + 100);
    check(s.renameIndex == row && IsWindowVisible(s.hwndRenameEdit), "slow name click opens actual rename editor");
    if (!reset()) return 3;
    event(WM_LBUTTONDOWN, MK_LBUTTON); pump(interval + 100);
    check(s.renameIndex < 0, "held first press cannot start rename");
    event(WM_LBUTTONUP);
    check(s.renameIndex < 0 && s.fileClick.RenamePending(), "long press release starts a fresh rename wait");
    CancelFileClick(s);
    if (!reset()) return 3;
    click();
    HWND other = CreateWindowW(L"STATIC", L"Pulse isolated focus fixture", WS_OVERLAPPEDWINDOW,
        20, 20, 240, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(other, SW_SHOW); SetForegroundWindow(other);
    pump(interval + 100);
    check(GetForegroundWindow() == other && !s.fileClick.RenamePending() && s.renameIndex < 0,
        "real deactivation cancels rename without stealing foreground");
    DestroyWindow(other); SetForegroundWindow(s.hwnd); pump(50);
    if (!reset()) return 3;
    s.appPrefs.single_click_open = true;
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    auto reordered = std::make_shared<std::vector<fs::DirEntry>>(*ActiveTab(s)->snapshot);
    std::swap((*reordered)[0], (*reordered)[1]);
    ActiveTab(s)->snapshot = reordered;
    ++ActiveTab(s)->view_generation;
    event(WM_LBUTTONUP);
    trace_path("reordered-release", root);
    check(ActiveTab(s)->current_path == root,
        "replacement snapshot reordered between press and release prevents stale single open");
    if (!reset()) return 3;
    s.appPrefs.single_click_open = true;
    click(); event(WM_LBUTTONDBLCLK, MK_LBUTTON); event(WM_LBUTTONUP); pump(150);
    trace_path("single-mode-double", folder);
    check(ActiveTab(s)->current_path == folder,
        "single-click folder double click navigates only one level");
    for (int modifier : {VK_CONTROL, VK_SHIFT}) {
        if (!reset()) return 3;
        BYTE original[256]{}; GetKeyboardState(original);
        BYTE changed[256]{}; memcpy(changed, original, sizeof(changed)); changed[modifier] |= 0x80;
        SetKeyboardState(changed);
        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
        SetKeyboardState(original);
        check(!s.fileClick.RenamePending(), modifier == VK_CONTROL ? "Ctrl selection never arms rename" : "Shift selection never arms rename");
    }
    if (!reset()) return 3;
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
    event(WM_LBUTTONUP); pump(interval + 100);
    check(!s.fileClick.Item().Valid() && s.renameIndex < 0, "cancelled capture cannot later start rename");
    if (!reset()) return 3;
    event(WM_KEYDOWN, VK_F2);
    check(s.renameIndex == row && IsWindowVisible(s.hwndRenameEdit), "F2 still opens explicit rename immediately");
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    return failures ? 1 : 0;
}
