#include "window_show_watch.h"
#include <algorithm>

namespace pulse::dialog_launcher {
namespace {
thread_local WindowShowWatch* t_watch = nullptr;
}

WindowShowWatch::~WindowShowWatch() {
    if (hook_) UnhookWinEvent(hook_);
    if (t_watch == this) t_watch = nullptr;
}

bool WindowShowWatch::Start(Classifier classify) noexcept {
    if (hook_) return true;
    if (t_watch) { SetLastError(ERROR_ALREADY_INITIALIZED); return false; }
    try { pending_.reserve(kMaxPending); } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    classify_ = classify;
    t_watch = this;
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &WindowShowWatch::OnEvent, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook_) {
        const DWORD error = GetLastError();
        t_watch = nullptr;
        SetLastError(error ? error : ERROR_GEN_FAILURE);
        return false;
    }
    return true;
}

void CALLBACK WindowShowWatch::OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (event != EVENT_OBJECT_SHOW || object != OBJID_WINDOW || child != CHILDID_SELF || !hwnd) return;
    if (WindowShowWatch* watch = t_watch) watch->Note(hwnd);
}

bool WindowShowWatch::TopLevel(HWND hwnd) noexcept {
    return hwnd && GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow() && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD);
}

void WindowShowWatch::Note(HWND hwnd) noexcept {
    if (!TopLevel(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    const bool classified = classify_ && classify_(hwnd);
    ++accepted_;
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const Shown& item) { return item.pid == pid; });
    if (found != pending_.end()) { found->classified |= classified; return; }
    if (pending_.size() < kMaxPending) pending_.push_back({pid, classified});
}

void WindowShowWatch::Wait(const HANDLE* handles, DWORD count, DWORD timeout_ms) noexcept {
    if (!hook_) {
        if (count) WaitForMultipleObjects(count, handles, FALSE, timeout_ms);
        else Sleep(timeout_ms);
        return;
    }
    MsgWaitForMultipleObjectsEx(count, count ? handles : nullptr, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    // Out-of-context events are delivered while this thread retrieves messages.
    // Bounded, so a burst cannot starve the caller's loop.
    MSG message{};
    for (int i = 0; i < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::vector<WindowShowWatch::Shown> WindowShowWatch::Take() {
    std::vector<Shown> taken;
    taken.swap(pending_);
    pending_.reserve(kMaxPending);
    return taken;
}

} // namespace pulse::dialog_launcher
