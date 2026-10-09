// app_navigation.h — Navigation, watches, search, and virtual places.
#pragma once
#include "app_runtime.h"

namespace pulse {
constexpr UINT_PTR kTimerUncProbeRetry = 0x554e;
std::vector<std::wstring> CollectPanePaths(const AppState& s);
std::vector<ui::ViewMode> CollectPaneViews(const AppState& s);
bool IsUncPath(const std::wstring& p);
void PumpUncProbe(AppState& s);
void RequestUncProbe(AppState& s, std::wstring unc);
void ProbePinnedNetworks(AppState& s);
void WarmupUnc(AppState& s, const std::wstring& path);
std::wstring PinCandidate(AppState& s);
std::wstring ProjectSearchRoot(AppState& s);
void OpenWorkspace(AppState& s, int index);
index::Query MakeSearchPageQuery(const app::Tab& tab, const std::wstring& rest,
                                        size_t offset);
void DispatchIndexSearch(AppState& s, const index::Query& query, uint32_t id);
void RequestSearchPage(AppState& s, app::Tab& tab, const std::wstring& rest,
                              bool reset);
void ApplySearchHits(app::Tab& tab, const std::wstring& rest,
                            index::SearchResult&& result);
void ConfigureContentSort(const app::Tab& tab,index::ContentSearchRequest& request);
void RequestSavedSearch(AppState& s, app::Tab& tab, size_t saved_index);
void ApplyContentSearchUpdate(AppState& s, index::ContentSearchUpdate update);
void DeliverIndexSearchResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network_snapshot = false);
void AcceptIndexProviderResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network,
                                      bool network_final = true);
void AcceptLiveNetworkProgress(AppState& s, const std::shared_ptr<LiveNetworkSearch>& live);
void DropLiveNetworkSearch(AppState& s, uint64_t session_id);
void AddLiveNetworkRoot(AppState& s, app::Tab& tab);
void MaybePrefetchSearchPage(AppState& s);
void CancelActiveContentSearch(AppState& s, app::Tab& tab);
enum class PathLoadReason { Navigate, RestoreSession, History };
void MarkContentSearchStopped(app::Tab& tab);
void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path,
                     PathLoadReason reason = PathLoadReason::Navigate);
// Saved "group by" for a folder, else the Downloads/Recent Date default.
int FolderGroupFor(const AppState& s, const std::wstring& path);
void StartLoadingPath(AppState& s, app::Tab& tab, const std::wstring& path,
                      PathLoadReason reason = PathLoadReason::Navigate);
void ApplyWorkerResult(AppState& s, app::WorkResult& res);
void ProcessPendingResults(AppState& s);
void CaptureListingSelection(app::Tab& tab);
bool PathHasPendingRefresh(AppState& s, const std::wstring& path);
enum class RefreshReason { Explicit, Background, ShellNotification, OperationCompleted, FileChange };
void RefreshPath(AppState& s, const std::wstring& path, RefreshReason reason = RefreshReason::Background);
void RevalidateVisibleFolders(AppState& s);
void RefreshActiveTab(AppState& s, RefreshReason reason = RefreshReason::Explicit);
void QueueSnapshotValidation(AppState& s, app::Tab& tab);
bool ApplyNotifyToVisible(AppState& s, const std::wstring& path,
                                 const fs::DirNotifyEvent& event);
void DropSizePatches(AppState& s, const std::wstring& path);
void QueueSizePatch(AppState& s, const std::wstring& path, const std::wstring& name,
                           ULONGLONG due);
void DrainDirNotifies(AppState& s);
void RestoreNavigationReturnSelection(AppState& s, app::Tab& tab,
                                             const std::wstring& childName);
void NavigateTo(AppState& s, const std::wstring& path);
void FocusPane(AppState& s, app::Pane* p);
void ApplyLayoutPreset(AppState& s, app::LayoutPreset preset);
// Two-pane layouts: the panes trade places (tabs, history, selection and view go
// with them); the divider position stays.
void SwapSplitPanes(AppState& s);
// Two-pane folder compare.
void UpdateFolderCompare(AppState& s);
void SetFolderCompare(AppState& s, bool on);
void ToggleCompareDiffOnly(AppState& s);
bool FolderCompareAvailable(AppState& s);
std::vector<std::wstring> SideBySideFolders(const app::Tab& tab);
void OpenFoldersSideBySide(AppState& s);
void TransferToTarget(AppState& s, bool move);
void CycleFocus(AppState& s);
void MarkTargetPane(AppState& s);
void SortBy(AppState& s, ui::SortColumn col);
void SetSort(AppState& s, ui::SortColumn col, ui::SortDirection direction);
// "Group by" for the active folder (remembered per folder); 0 turns it off.
void SetGroupBy(AppState& s, int group_by);
void OpenSelected(AppState& s);
// Opens one path as if chosen in the list: folders navigate, files launch.
void OpenPath(AppState& s, const std::wstring& path);
void GoUp(AppState& s);
void GoBack(AppState& s);
void GoForward(AppState& s);
bool IsSettingsTab(const app::Tab* tab);
std::wstring NewTabPath(const AppState& s);
// A tab the user asked for (Ctrl+T, the + buttons): opens NewTabPath, where an
// empty path means This PC rather than NewTab's C:\ fallback.
void OpenNewTab(AppState& s);
// Opens `path` in a new active tab; empty means This PC (not C:\\).
void OpenTabAt(AppState& s, const std::wstring& path);
void NewTab(AppState& s, const std::wstring& path);
void NewBackgroundTab(AppState& s, const std::wstring& path);
void OpenSettingsTab(AppState& s, int page);
void CloseLayoutTab(AppState& s, size_t idx);
void CloseActiveTab(AppState& s);
void SwitchTab(AppState& s, size_t idx);
// Multi-window tear-off: moves the tab at `idx` into a new top-level window
// (the tab menu's "move to new window"), leaving the source window's other
// tabs alone. Returns the new window's handle, or null when the tab has no
// folder to reopen (settings, search results).
HWND TearOffLayoutTab(AppState& s, size_t idx);
bool ActivateExistingFolderTab(AppState& s, const std::wstring& path);
// "Open in new tab": switches to (and pulses) a tab already showing `path`
// instead of duplicating it.
void OpenFolderTab(AppState& s, const std::wstring& path);
// Selects `name` in `tab` now when listed, else once its listing lands.
void SelectNameInTab(AppState& s, app::Tab& tab, const std::wstring& name);
float TabFlashAmount(const AppState& s, const app::LayoutTab* key);
// Publishes the tag catalog for tag grouping; true when it changed.
bool SyncTagGroups(AppState& s);
// Advances the pulse; false once it has finished (or none is running).
bool TickTabFlash(AppState& s);
} // namespace pulse
