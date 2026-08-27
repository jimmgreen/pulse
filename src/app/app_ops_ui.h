// app_ops_ui.h — Delete/tray/paste/conflict/recycle operations UI.
#pragma once
#include "app_runtime.h"

namespace pulse {
bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request);
void ReleaseTrayBatch(AppState& s, size_t idx);
void PasteIntoCurrent(AppState& s);
void DeleteSelected(AppState& s, bool permanent);
void RestoreSelected(AppState& s);
void EmptyRecycleBin(AppState& s);
void CollectToTray(AppState& s, bool move_intent);
void ShowBatchRename(AppState& s);
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog);
} // namespace pulse
