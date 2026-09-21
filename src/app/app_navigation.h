// app_navigation.h — Navigation, watches, search, and virtual places.
#pragma once
#include "app_runtime.h"

namespace pulse {
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
                                     index::SearchResult&& result);
void AcceptIndexProviderResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network);
void MaybePrefetchSearchPage(AppState& s);
void CancelActiveContentSearch(AppState& s, app::Tab& tab);
enum class PathLoadReason { Navigate, RestoreSession };
void MarkContentSearchStopped(app::Tab& tab);
void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path,
                     PathLoadReason reason = PathLoadReason::Navigate);
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
void TransferToTarget(AppState& s, bool move);
void CycleFocus(AppState& s);
void MarkTargetPane(AppState& s);
void SortBy(AppState& s, ui::SortColumn col);
void SetSort(AppState& s, ui::SortColumn col, ui::SortDirection direction);
void OpenSelected(AppState& s);
void GoUp(AppState& s);
void GoBack(AppState& s);
void GoForward(AppState& s);
bool IsSettingsTab(const app::Tab* tab);
std::wstring NewTabPath(const AppState& s);
void NewTab(AppState& s, const std::wstring& path);
void OpenSettingsTab(AppState& s, int page);
void CloseLayoutTab(AppState& s, size_t idx);
void CloseActiveTab(AppState& s);
void SwitchTab(AppState& s, size_t idx);
bool ActivateExistingFolderTab(AppState& s, const std::wstring& path);

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
