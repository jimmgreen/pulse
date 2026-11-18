// app_runtime.h — Shared layout accessors and window-model helpers.
#pragma once
#include "app_state.h"
#include "split_tree_shape.h"

namespace pulse {

inline constexpr UINT_PTR kStatusFeedbackTimer = 0x53544154;
inline void ShowStatusFeedback(AppState& s, std::wstring text) {
    s.status_feedback = std::move(text);
    SetTimer(s.hwnd, kStatusFeedbackTimer, 3500, nullptr);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

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

// The preset a pinned workspace keeps for the active tab, which knows no
// free split tree: a custom one (M7k) as the preset with as many panes.
inline app::LayoutPreset WorkspaceLayoutOf(AppState& s) {
    const app::LayoutTab& tab = LiveLayout(s);
    return tab.layout == app::LayoutPreset::Custom ? app::PresetForPaneCount(tab.VisiblePanes().size()) : tab.layout;
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
                                    OpenPath, SearchSubfolders, LanRescan };
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
std::wstring ResolveOpenFolderPath(std::wstring path, bool reveal = false);
// When `raw` (a launch or forwarded path) names an existing file, selects it
// in the active tab, which ResolveOpenFolderPath opened on its folder.
void SelectLaunchedFile(AppState& s, const std::wstring& raw, bool reveal = false);
void OpenFolderInNewTab(AppState& s, const std::wstring& raw, bool reveal = false);
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
bool ApplyQueriedRecycleInfo(AppState& s, const fs::RecycleBinInfo& info, uint64_t request = 0);
void OnRecycleShellChanged(AppState& s, WPARAM wparam, LPARAM lparam);
void ReleaseRecycleShellWatch(AppState& s, HWND hwnd);
void RefreshRecycleViews(AppState& s, bool query_occupancy = true);
void ScheduleRecycleRefresh(AppState& s);
bool PumpRecycleRefresh(AppState& s, ULONGLONG now);
void BumpRecycleOccupancy(AppState& s, int64_t delta);
void ClearRecycleOccupancy(AppState& s);
void ApplyEmptyRecycleFinished(AppState& s, bool emptied);
bool ToggleStarred(AppState& s, const std::wstring& target,
                          app::PlaceItemKind kind = app::PlaceItemKind::Unknown);
void StopDetailsSizeWalk(AppState& s);
void StartDetailsSizeWalk(AppState& s, const std::wstring& path);
void ShutdownDetailsSizeWalk(AppState& s);
std::wstring DetailsAttributeText(DWORD attrs);
bool TickTrayDeck(AppState& s);
// Shelf release mode right now: the tray-wide mode, flipped while Shift is held.
bool TrayEffectiveMove(const AppState& s);
// Hashes of every staged path ("已暂存" row pills), cached per tray state.
std::shared_ptr<const std::unordered_set<uint64_t>> StagedHashes(AppState& s);
ui::WindowViewModel BuildVm(AppState& s, bool probe_details = true);
std::wstring TooltipForHover(AppState& s);
// Records what the pointer is over. Both mouse-move paths (client and frame)
// must call this, or a hover field silently stops updating.
void ApplyHoverTarget(AppState& s, const ui::HitTestResult& hit);
std::wstring EntryFullPath(const app::Tab& tab, int index);
std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab);
// Phone folders (pulse:device:): their file commands are Explorer's own verbs,
// run by the shell host on the rows' shell ids (DirEntry::shell_id).
bool IsDeviceTab(const app::Tab* tab);
// Shell ids of the selected rows; empty if any selected row has none.
std::vector<std::wstring> SelectedShellIds(const app::Tab& tab);
// Queues the verb (empty = the default verb); false when nothing was queued.
bool RunDeviceVerb(AppState& s, std::vector<std::wstring> shell_ids, const wchar_t* verb,
                   bool background = false);
// Copy / move into or out of a phone (ops::IsShellTransferRequest): sources
// are phone rows' shell ids or paths, dest a Pulse folder (pulse:device: or
// local). Pulse's task: progress card, conflicts in Pulse's dialog, the
// folders re-listed when it ends. False when nothing was queued.
bool SubmitDeviceTransfer(AppState& s, std::vector<std::wstring> sources, const std::wstring& dest,
                          bool move, uint64_t* task = nullptr);
// The phone folder the window's active tab shows is watched like Explorer
// watches it (SHChangeNotifyRegister): what another program or the phone's
// own shell changes there re-lists it. Called when a listing is applied; the
// folder's PIDL is parsed off the UI thread (the device may be slow).
void UpdateDeviceFolderWatch(AppState& s);
void OnDeviceWatchReady(AppState& s, WPARAM ticket, LPARAM pidl);
void OnDeviceFolderChanged(AppState& s, WPARAM wparam, LPARAM lparam);
void FireDeviceFolderWatch(AppState& s);   // kTimerDeviceWatch: the burst settled
void ReleaseDeviceFolderWatch(HWND hwnd);
// What Explorer's 复制路径 gives for a phone item: 此电脑\Xiaomi 14\内部存储\DCIM\a.jpg.
std::wstring DeviceFriendlyPath(const std::wstring& path);
std::wstring TagDiscoveryKey(std::wstring path);
void QueueVisibleTagDiscovery(AppState& s);
void ApplyTagAdsDiscoveries(AppState& s, const std::vector<TagAdsDiscovery>& discoveries);
std::wstring SelectedFullPath(AppState& s);
void SyncSavedSearchSidebar(AppState& s);
} // namespace pulse

namespace pulse {
void StartSettingsSearch(AppState&);
void LayoutSettingsSearch(AppState&);
void SyncSettingsSearch(AppState&);
void FillSettingsSearchVm(AppState&, ui::WindowViewModel&);
void OpenSettingsSearchResult(AppState&, int);
}
