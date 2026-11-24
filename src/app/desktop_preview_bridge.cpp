#include "desktop_preview_bridge.h"
#include "desktop_preview_session.h"
#include "app_state.h"
#include "app_commands.h"
#include "../ui/window_effect_support.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse {
struct DesktopPreviewBridge::Impl {
    static constexpr UINT kOpen = WM_APP + 1, kCommand = WM_APP + 2;
    struct Result { std::atomic<unsigned> in_flight{0}; std::mutex mutex; uint64_t generation = 0; bool ready = false; ui::QuickPreviewItem item; };
    HWND owner = nullptr, dispatcher = nullptr;
    DesktopPreviewSession session;
    std::shared_ptr<ui::QuickPreviewWindow> preview = std::make_shared<ui::QuickPreviewWindow>();
    RECT origin{};
    bool shown = false;
    HWND request_foreground = nullptr;
    uint64_t waiting_generation = 0;
    std::shared_ptr<Result> result = std::make_shared<Result>();
    explicit Impl(HWND hwnd) : owner(hwnd) {
        WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Pulse.DesktopPreview.Dispatch";
        RegisterClassW(&wc);
        dispatcher = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, wc.hInstance, this);

    }
    ~Impl() { session.Reset(); if (preview) preview->Close(); if (dispatcher) DestroyWindow(dispatcher); }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (msg == WM_TIMER) {
                self->Tick();
                if (!self->session.pending && !self->session.hover && !self->preview->visible() &&
                    !self->session.Accept(self->waiting_generation)) KillTimer(hwnd, 1);
                return 0;
            }
            if (msg == kOpen || msg == kCommand) {
                if (auto* app = GetAppState(self->owner)) {
                    ui::QuickPreviewCommand command;
                    if (msg == kOpen) {
                        command.action = ui::QuickPreviewAction::Open;
                        command.target = self->preview->item();
                        auto path = self->preview->TakeOpenPath();
                        if (!path.empty()) command.target.path = std::move(path);
                    } else if (!self->preview->TakeCommand(static_cast<UINT_PTR>(lp), command)) return 0;
                    if (command.action == ui::QuickPreviewAction::PinDetach) {
                        self->session.Reset();
                        self->shown = false;
                        auto detached = std::exchange(self->preview, std::make_shared<ui::QuickPreviewWindow>());
                        DetachQuickPreviewToPinned(*app, std::move(detached));
                        return 0;
                    }
                    command.folder_child = true; // path-based actions, never the pane selection
                    self->session.hover = false;
                    auto keep = self->preview;
                    HandleQuickPreviewCommand(*app, *keep, command);
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    void Tick() {
        auto* app = IsWindow(owner) ? GetAppState(owner) : nullptr;
        if (!app) { session.Reset(); preview->Close(); KillTimer(dispatcher, 1); return; }
        if (shown && !preview->visible()) { session.Reset(); shown = false; }
        if (preview->visible() && GetForegroundWindow() == preview->hwnd()) session.hover = false;
        POINT cursor{}; GetCursorPos(&cursor);
        RECT popup{}; if (preview->visible()) GetWindowRect(preview->hwnd(), &popup);
        const bool hover_left = !PtInRect(&origin, cursor) && !PtInRect(&popup, cursor);
        if (session.hover && (app->appPrefs.preview_trigger != 0 || app->safeMode || session.Outside(hover_left, preview->visible(), GetTickCount64()) ||
            (GetForegroundWindow() != request_foreground && GetForegroundWindow() != preview->hwnd()))) {
            session.Cancel(session.path); preview->Close();
        }
        if (session.Ready(GetTickCount64()) && result->in_flight.load() < 2) {
            session.pending = false;
            auto shared = result; const auto generation = session.generation; const auto path = session.path;
            waiting_generation = generation;
            shared->in_flight.fetch_add(1);
            try { std::thread([shared, generation, path] {
                struct Completion { std::shared_ptr<Result> state; ~Completion() { state->in_flight.fetch_sub(1); } } completion{shared};
                WIN32_FILE_ATTRIBUTE_DATA data{};
                GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
                ui::QuickPreviewItem item; item.path = path;
                item.name = path.substr(path.find_last_of(L"\\/") + 1);
                item.attrs = data.dwFileAttributes;
                item.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                item.modified = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                std::lock_guard lock(shared->mutex);
                if (generation >= shared->generation) { shared->generation = generation; shared->ready = true; shared->item = std::move(item); }
            }).detach(); } catch (...) {
                shared->in_flight.fetch_sub(1); waiting_generation = 0; session.Reset();
            }
        }
        ui::QuickPreviewItem item;
        { std::lock_guard lock(result->mutex);
          if (!result->ready || !session.Accept(result->generation)) return;
          item = std::move(result->item); result->ready = false; result->item = {}; }
        waiting_generation = 0;
        if (item.path.empty()) { session.Reset(); return; }
        if (!preview->Initialize(owner, 0, kOpen, kCommand, dispatcher, true)) return;
        auto options = QuickPreviewOptionsFromPrefs(app->appPrefs);
        if (session.hover) {
            const int size = app::NormalizeHoverCardSize(app->appPrefs.hover_card_size);
            options.size_mode = 1; options.remember_size = true;
            options.remembered = RECT{0, 0, 360 + size * 160, 300 + size * 120};
        }
        preview->SetOptions(options); preview->SetAccent(app->accentColor);
        const POINT point{static_cast<LONG>((int64_t(origin.left) + origin.right) / 2),
                          static_cast<LONG>((int64_t(origin.top) + origin.bottom) / 2)};
        const auto occupied = session.hover ? std::vector<RECT>{} : VisiblePinnedPreviewRects(app->core);
        shown = true;
        preview->Show(item, app->darkMode, ui::WindowEffectFromId(app->appPrefs.window_effect),
                      app->safeMode, &point, !session.hover, occupied.empty() ? nullptr : &occupied);
        if (session.hover) {
            RECT window{}; GetWindowRect(preview->hwnd(), &window);
            MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &mi);
            const LONG x = std::clamp(static_cast<LONG>((std::min)(int64_t(origin.right) + 12, int64_t(INT32_MAX))), mi.rcWork.left, (std::max)(mi.rcWork.left, mi.rcWork.right - (window.right-window.left)));
            const LONG y = std::clamp(origin.top, mi.rcWork.top, (std::max)(mi.rcWork.top, mi.rcWork.bottom - (window.bottom-window.top)));
            SetWindowPos(preview->hwnd(), nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
};
DesktopPreviewBridge::DesktopPreviewBridge(HWND owner) : impl_(std::make_unique<Impl>(owner)) {}
DesktopPreviewBridge::~DesktopPreviewBridge() = default;
bool DesktopPreviewBridge::Request(const std::wstring& path, unsigned mode, const RECT* origin) {
    auto& s = *impl_; auto* app = IsWindow(s.owner) ? GetAppState(s.owner) : nullptr;
    if (!app || !s.dispatcher) return false;
    SetTimer(s.dispatcher, 1, 25, nullptr);
    if (s.preview->visible() && GetForegroundWindow() == s.preview->hwnd()) s.session.hover = false;
    if (mode == 5) {
        POINT cursor{}; RECT popup{}; GetCursorPos(&cursor);
        if (s.preview->visible()) GetWindowRect(s.preview->hwnd(), &popup);
        if (s.session.hover && path == s.session.path &&
            s.session.Outside(!PtInRect(&popup, cursor), s.preview->visible(), GetTickCount64()) &&
            s.session.Cancel(path)) s.preview->Close();
        return true;
    }
    if (mode == 4 && (app->appPrefs.preview_trigger != 0 || app->safeMode)) {
        if (s.session.Cancel(s.session.path)) s.preview->Close();
        return true;
    }
    if (s.shown && !s.preview->visible()) { s.session.Reset(); s.shown = false; }
    if (origin) s.origin = *origin;
    if (s.session.Request(path, mode == 4, app->appPrefs.preview_trigger == 0,
        GetTickCount64(), static_cast<unsigned>(app::NormalizeHoverPreviewDelay(app->appPrefs.hover_preview_delay_ms)))) {
        s.preview->Close(); s.shown = false; s.request_foreground = GetForegroundWindow(); s.Tick();
    }
    return true;
}
}

