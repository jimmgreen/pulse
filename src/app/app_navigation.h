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
void RequestSavedSearch(AppState& s, app::Tab& tab, size_t saved_index);
void ApplyContentSearchUpdate(AppState& s, index::ContentSearchUpdate update);
void DeliverIndexSearchResult(AppState& s, uint32_t id,
                                     index::SearchResult&& result);
void AcceptIndexProviderResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network);
void MaybePrefetchSearchPage(AppState& s);
void CancelActiveContentSearch(AppState& s, app::Tab& tab);
void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path);
void StartLoadingPath(AppState& s, app::Tab& tab, const std::wstring& path);
void ApplyWorkerResult(AppState& s, app::WorkResult& res);
void ProcessPendingResults(AppState& s);
void CaptureListingSelection(app::Tab& tab);
bool PathHasPendingRefresh(AppState& s, const std::wstring& path);
void RefreshPath(AppState& s, const std::wstring& path);
void RevalidateVisibleFolders(AppState& s);
void RefreshActiveTab(AppState& s);
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
} // namespace pulse
