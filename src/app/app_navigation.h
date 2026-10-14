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
// The window a shared search result is for: the one whose tab, palette or
// advanced-search count waits for request `id`, else `s`. The search services
// are shared and the host hands their results to the most recent window,
// which need not be the one that searched (M5a).
AppState& SearchResultWindow(AppState& s, uint32_t id);
// The same for a content search update (its own generation counter).
AppState& ContentSearchWindow(AppState& s, uint64_t generation);
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
// A folder was moved or renamed (M6e): every tab in every window showing it,
// or a folder inside it, follows to the new path with its rows, selection and
// history, then re-lists in the background.
void FollowMovedFolder(AppState& s, const std::wstring& from, const std::wstring& to);
void RefreshActiveTab(AppState& s, RefreshReason reason = RefreshReason::Explicit);
// A BitLocker unlock prompt finished: reload the tabs on that drive and This PC.
void ApplyBitLockerUnlock(AppState& s, const std::wstring& root, bool unlocked);
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
// Item menu "复制到窗口 / 移动到窗口" (M6e): the writable real folders the
// panes of the other windows show, the window used last first and in each
// window its focused pane first, without repeats or the folder the selection
// is in; and sending the selection to one of them.
std::vector<std::wstring> WindowTransferFolders(AppState& s);
void TransferSelectionToFolder(AppState& s, const std::wstring& folder, bool move);
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
bool ActivateExistingFolderTab(AppState& s, const std::wstring& path);
// Whether a visible pane of one of the window's tabs shows `path` (what
// ActivateExistingFolderTab would bring forward).
bool ShowsFolder(AppState& s, const std::wstring& path);
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

// Remembers `outgoing`, which has just stopped being the active tab, as the
// last tab used inside its group. The group chip's hover card draws that tab
// with a faint check when the group no longer owns the active tab. Passing a
// tab that is still active is harmless (it is the most recent one in its
// group); passing an ungrouped or null tab records nothing.
void RememberGroupActivation(AppState& s, const app::LayoutTab* outgoing);
// Drops remembered tabs that are no longer members of their group, which covers
// tabs closed inside the tab controller and groups that lost their members.
// Addresses are only compared, so a stale pointer is never dereferenced.
void PruneGroupActivations(AppState& s);
} // namespace pulse
