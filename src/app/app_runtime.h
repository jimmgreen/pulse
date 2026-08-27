// app_runtime.h — Shared layout accessors and window-model helpers.
#pragma once
#include "app_state.h"

namespace pulse {

inline app::LayoutTab& LiveLayout(AppState& s) {
    s.window_tabs.EnsureDefault();
    return *s.window_tabs.Active();
}

inline const app::LayoutTab* LiveLayout(const AppState& s) {
    return s.window_tabs.Active();
}

inline std::vector<std::unique_ptr<app::Pane>>& Panes(AppState& s) {
    return LiveLayout(s).panes;
}

inline std::unique_ptr<app::SplitContainer>& Root(AppState& s) {
    return LiveLayout(s).root;
}

inline const std::unique_ptr<app::SplitContainer>& Root(const AppState& s) {
    static const std::unique_ptr<app::SplitContainer> empty;
    const app::LayoutTab* tab = LiveLayout(s);
    return tab ? tab->root : empty;
}

inline app::LayoutPreset& LayoutOf(AppState& s) {
    return LiveLayout(s).layout;
}

template<typename Fn>
inline void ForEachPane(AppState& s, Fn&& fn) {
    for (auto& owned : s.window_tabs.items) {
        if (!owned) continue;
        for (auto& pane : owned->panes) {
            if (pane) fn(*pane);
        }
    }
}

inline app::Tab* ActiveTab(AppState& s) {
    return s.pane ? s.pane->ActiveTab() : nullptr;
}

void PrefetchDetailsMeta(HWND hwnd, const std::wstring& path);
void RememberLayoutFocus(AppState& s);
std::vector<std::wstring> VisibleFolderPaths(const AppState& s);
void SyncVisibleWatches(AppState& s);
void BindCurrentLayout(AppState& s);
std::wstring ResolveOpenFolderPath(std::wstring path);
void OpenFolderInNewTab(AppState& s, const std::wstring& raw);
void PostWorkerResult(AppState& s, app::WorkResult res);
D2D1_RECT_F FocusedPaneRect(const AppState& s);
app::Pane* PaneAtSlot(AppState& s, int index);
D2D1_RECT_F ListRect(const AppState& s);
bool ScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                              D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& maxScroll);
bool HorizontalScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                                         D2D1_RECT_F& track, D2D1_RECT_F& thumb,
                                         float& maxScroll);
void RememberPath(AppState& s, const std::wstring& path);
void RecordRecentOpen(AppState& s, const std::wstring& path,
                             app::PlaceItemKind kind);
void FillPaneSlots(AppState& s, ui::WindowViewModel& vm);
int TrayDeckCap(const AppState& s);
std::vector<TrayDeckEntry> TrayDeckEntries(const app::StagingTray& tray, size_t offset,
                                                  size_t cap);
int TrayItemTotalCount(const app::StagingTray& tray);
std::wstring TrayDisplayPath(const std::wstring& path);
std::wstring TrayItemName(const std::wstring& path);
int TrayDeckHoverIndex(const AppState& s);
void RefreshStarredViews(AppState& s);
void RefreshRecentViews(AppState& s);
bool IsRecycleTab(const app::Tab* tab);
std::wstring RecycleOccupancyText(const fs::RecycleBinInfo& info);
void ApplyRecycleOccupancy(AppState& s);
void RequestRecycleOccupancy(AppState& s);
void RefreshRecycleViews(AppState& s);
bool ToggleStarred(AppState& s, const std::wstring& target,
                          app::PlaceItemKind kind = app::PlaceItemKind::Unknown);
void StopDetailsSizeWalk(AppState& s);
void StartDetailsSizeWalk(AppState& s, const std::wstring& path);
void ShutdownDetailsSizeWalk(AppState& s);
std::wstring DetailsAttributeText(DWORD attrs);
bool TickTrayDeck(AppState& s);
ui::WindowViewModel BuildVm(AppState& s, bool probe_details = true);
std::wstring TooltipForHover(AppState& s);
std::wstring EntryFullPath(const app::Tab& tab, int index);
std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab);
std::wstring TagDiscoveryKey(std::wstring path);
void QueueVisibleTagDiscovery(AppState& s);
std::wstring SelectedFullPath(AppState& s);
void SyncSavedSearchSidebar(AppState& s);
} // namespace pulse
