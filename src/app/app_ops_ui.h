// app_ops_ui.h — Delete/tray/paste/conflict/recycle operations UI.
#pragma once
#include "app_runtime.h"
#include <optional>
#include <memory>
#include "../ui/confirm_dialog.h"

namespace pulse::app::rename { struct Preset; }
namespace pulse { struct DeletePreflight; }

namespace pulse {
bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request, uint64_t* task_id = nullptr);
void ReleaseTrayBatch(AppState& s, size_t idx);
void PasteIntoCurrent(AppState& s);
void DeleteSelected(AppState& s, bool permanent);
void PollDeletePreflight(AppState& s);
void CancelDeletePreflight(AppState& s);
std::shared_ptr<DeletePreflight> DeletePaths(AppState& s, std::vector<std::wstring> paths, bool permanent,
    bool recycle_only = false, bool force_confirmation = false);
// A delete finished: rows that slid out return if it failed; a recycle offers
// "Deleted N items - Undo" (motion 5b).
void NoteDeleteCompletion(AppState& s, const ops::CompletedOperation& completed);
void NoteDeleteFinished(AppState& s, const ops::FinishedTask& done);
// The batch rename dialog's task ended: flash the renamed rows and offer undo
// (once the dialog has closed).
void NoteBatchRenameFinished(AppState& s, const ops::FinishedTask& done);
// WM_RECYCLE_REFUSED: offers to delete for good what a recycle left because
// the Recycle Bin would not keep it, as DeletePaths asks before a delete.
void OfferPermanentDeleteForRefused(AppState& s);
// Confirmation shown before Delete moves items to the Recycle Bin (opt-in setting).
ui::ConfirmDialogSpec BuildRecycleDeleteConfirm(const std::vector<std::wstring>& paths);
void RestoreSelected(AppState& s);
// Restores every listed recycle-bin item (the filtered view when a filter is on).
void RestoreAllRecycle(AppState& s);
void EmptyRecycleBin(AppState& s);
void CollectToTray(AppState& s, bool move_intent);
// Ctrl+C / Ctrl+X: writes the system clipboard and arms s.trayClip (the
// shelf's 收入 offer). Nothing is staged.
void CollectPathsToTray(AppState& s, const std::vector<std::wstring>& paths, bool move_intent);
// The Ctrl+C offer still matches the clipboard; clears it once it does not.
bool TrayClipLive(AppState& s);
// Stages paths not already on the tray. move_mode sets the release mode;
// nullopt keeps it. Returns how many were added.
size_t StagePaths(AppState& s, const std::vector<std::wstring>& paths, std::optional<bool> move_mode);
// Ctrl+Shift+S / 加入暂存区: the selection, without touching the clipboard.
void StageSelection(AppState& s);
// 收入: stages the Ctrl+C offer.
void TakeTrayClip(AppState& s);
void ShowBatchRename(AppState& s);
// Staging tray recent drop destinations (appPrefs.tray_dests, newest first, max 5).
std::vector<std::wstring> TrayDestList(const AppState& s);
void RememberTrayDest(AppState& s, const std::wstring& dir);
// Copy (or move) every staged item that still exists into dir.
void SendTrayToDest(AppState& s, const std::wstring& dir, bool move);
// Shelf release: copy / move every staged item
// that exists and is not already in dir. Selects the arrivals when dir is the
// current folder; a snack with 撤销 / 打开 follows when the task finishes.
void ReleaseTrayTo(AppState& s, const std::wstring& dir, bool move);
void NoteTrayReleaseFinished(AppState& s, const ops::FinishedTask& done);
// Every staged path whose file still exists (shelf ZIP, rename, drag-out).
std::vector<std::wstring> TrayLivePaths(const AppState& s);
// Clears the tray; the snack offers to put the items back (WM_TRAY_RESTORE).
void ClearTrayWithUndo(AppState& s);
void HandleTrayMessage(AppState& s, UINT message);
// Removes staged items by id (open view Del).
void RemoveTrayItems(AppState& s, const std::vector<uint64_t>& ids);
// Removes one staged path (the "已暂存" row pill).
void UnstagePath(AppState& s, const std::wstring& path);
// Batch rename an explicit path list (staging tray); tray entries follow.
void ShowBatchRenamePaths(AppState& s, const std::vector<std::wstring>& paths);
// Batch rename opened with `preset` filled in (Pulse 助手's 在批量重命名中调整…);
// `select_in` keeps the renamed items selected. Returns how many files it
// renamed (0: closed without renaming).
size_t ShowBatchRenameWith(AppState& s, const std::vector<std::wstring>& paths, const app::rename::Preset& preset,
                           app::Tab* select_in);
// Builds this window's operations window the first time it has to show it.
bool EnsureOperationWindow(AppState& s);
void PinAndShowOperationWindow(AppState& s);
void HandleUndoHistoryAction(AppState& s, uint64_t task_id, bool undone, bool locate);
void HandleOperationStatusAction(AppState& s, int action);
void HandleOperationHistoryAction(AppState& s, uint64_t task_id, int action);
std::vector<ui::FluentMenuItem> BuildOperationHistoryMenu(AppState& s, const std::vector<ops::OpStatus>& history);
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog);
// Makes `to` the window that shows file operations (multi-window M4c, plan
// §8.1): ops notifications and shell dialogs go to it, and the progress
// window's state moves along, shown there without focus if it was showing.
void MoveOperationUi(AppState& from, AppState& to);
} // namespace pulse
