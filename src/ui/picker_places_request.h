#pragma once
#include "../common/picker_places_snapshot.h"
#include <future>
#include <functional>

namespace pulse::ui {
inline bool RunPickerPlacesRequest(HWND owner, std::function<bool()> operation) {
    std::future<bool> request;
    try {
        request = std::async(std::launch::async, std::move(operation));
    } catch (...) { return false; }
    const bool enabled = IsWindowEnabled(owner) != FALSE;
    if (enabled) EnableWindow(owner, FALSE);
    bool quit = false;
    int quit_code = 0;
    while (request.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        MSG message{};
        for (unsigned dispatched = 0; dispatched < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++dispatched) {
            if (message.message == WM_QUIT) { quit = true; quit_code = static_cast<int>(message.wParam); }
            else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    bool success = false;
    try { success = request.get(); } catch (...) {}
    if (enabled && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (quit) PostQuitMessage(quit_code);
    return success;
}
inline bool RunPickerFavoriteRequest(HWND owner, picker_places::Snapshot snapshot,
    std::vector<picker_places::StarredItem> items, std::wstring group,
    std::function<bool(const std::vector<picker_places::StarredItem>&, const std::wstring&)> mutate = {}) {
    return RunPickerPlacesRequest(owner, [snapshot = std::move(snapshot), items = std::move(items), group = std::move(group), mutate = std::move(mutate)] {
        return mutate ? mutate(items, group) : picker_places::RequestAddStarredBatch(snapshot, items, group);
    });
}
} // namespace pulse::ui
