// app_runtime.h — Shared layout accessors and window-model helpers.
#pragma once
#include "app_state.h"

namespace pulse {

// The updater must keep the window open if session/preferences cannot be saved.
bool PrepareSessionForUpdate(AppState& s);

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

// Clickable follow-up shown at the end of the status-bar context hint.
enum class StatusHintAction : int { None = 0, Tray, Compare, DiffOnly, ShowAll, Advanced, Shortcuts,
                                    OpenPath, SearchSubfolders };
// Re-runs the active tab's pane filter as a search of the folder and its subfolders.
void SearchFilterInSubfolders(AppState& s);
StatusHintAction CurrentStatusHintAction(AppState& s);
// Teaching bubbles: evaluate triggers (UI timer); returns true when a repaint is needed.
bool UpdateTeachTip(AppState& s);
// Bubble buttons: 0 = Got it / Try it, 1 = close, 2 = don't show tips.
void HandleTeachButton(AppState& s, int button);
std::vector<std::wstring> VisibleFolderPaths(const AppState& s);
void SyncVisibleWatches(AppState& s);
void BindCurrentLayout(AppState& s);
std::wstring ResolveOpenFolderPath(std::wstring path);
// A launch/forwarded argument (command line, single-instance hand-off, shell handoff):
// shell namespaces ("::{...}", "shell:") are translated to the Pulse view that answers
// them, or to nothing when Pulse has none; virtual views pass through; anything else is
// a path and gets ResolveOpenFolderPath.
std::wstring ResolveIncomingPath(const std::wstring& raw);
// When `raw` (a launch or forwarded path) names an existing file, selects it
// in the active tab, which ResolveOpenFolderPath opened on its folder.
void SelectLaunchedFile(AppState& s, const std::wstring& raw);
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
// Staging tray card stack (cyclic): index of the top card, throw the top card
// to the back (dir ±1, release offset in DIPs), bring the last card back on
// top, smoke burst for a dismiss, and exit hints for the next tick.
int TrayStackTop(const AppState& s);
void ThrowTrayTop(AppState& s, float dir, float dx, float dy);
void TrayStepBack(AppState& s);
void ReleaseTrayDrag(AppState& s, bool commit);
void SpawnTrayPuffs(AppState& s);
void MarkTrayExit(AppState& s, const std::vector<std::wstring>& paths, bool stagger);
void RefreshStarredViews(AppState& s);
void RefreshRecentViews(AppState& s);
bool IsRecycleTab(const app::Tab* tab);
std::wstring RecycleOccupancyText(const fs::RecycleBinInfo& info);
void ApplyRecycleOccupancy(AppState& s);
void RequestRecycleOccupancy(AppState& s);
bool ApplyQueriedRecycleInfo(AppState& s, const fs::RecycleBinInfo& info);
void RefreshRecycleViews(AppState& s, bool query_occupancy = true);
void ScheduleRecycleRefresh(AppState& s);
bool PumpRecycleRefresh(AppState& s, ULONGLONG now);
void BumpRecycleOccupancy(AppState& s, int64_t delta);
void ClearRecycleOccupancy(AppState& s);
bool ToggleStarred(AppState& s, const std::wstring& target,
                          app::PlaceItemKind kind = app::PlaceItemKind::Unknown);
void StopDetailsSizeWalk(AppState& s);
void StartDetailsSizeWalk(AppState& s, const std::wstring& path);
void ShutdownDetailsSizeWalk(AppState& s);
std::wstring DetailsAttributeText(DWORD attrs);
bool TickTrayDeck(AppState& s);
ui::WindowViewModel BuildVm(AppState& s, bool probe_details = true);
std::wstring TooltipForHover(AppState& s);
// Records what the pointer is over. Both mouse-move paths (client and frame)
// must call this, or a hover field silently stops updating.
void ApplyHoverTarget(AppState& s, const ui::HitTestResult& hit);
std::wstring EntryFullPath(const app::Tab& tab, int index);
std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab);
std::wstring TagDiscoveryKey(std::wstring path);
void QueueVisibleTagDiscovery(AppState& s);
void ApplyTagAdsDiscoveries(AppState& s, const std::vector<TagAdsDiscovery>& discoveries);
std::wstring SelectedFullPath(AppState& s);
void SyncSavedSearchSidebar(AppState& s);
} // namespace pulse
