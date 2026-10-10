// app_ops_ui.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "locked_item_prompt.h"
#include "paste_source.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace pulse;

namespace pulse {
namespace {
// Ops and Shell host errors are Simplified protocol text; zh-TW shows the
// converted form, so classification accepts both.
// Errors come from OpsManager (localized with Pick) or pulse_shell (Simplified),
// so accept the Simplified, Traditional and English forms.
bool ErrorIs(const std::wstring& error, const wchar_t* simplified, const wchar_t* english) {
    return error == simplified || error == l10n::Cn(simplified) || error == english;
}
bool ErrorHas(const std::wstring& error, const wchar_t* simplified) {
    return error.find(simplified) != std::wstring::npos ||
           error.find(l10n::Cn(simplified)) != std::wstring::npos;
}
} // namespace

bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request, uint64_t* task_id) {
    // Copy/move conflict discovery is part of the transfer worker's recursive
    // scan. The UI only consumes immutable conflict snapshots.
    const auto id = s.ops.Submit(std::move(request));
    if (task_id) *task_id = id;
    return id != 0;
}

// Tell the user that staged items which vanished were left out.
static void NotifyTraySkipped(AppState& s, size_t n) {
    if (n == 0) return;
    std::wstring msg = l10n::Get(l10n::StringId::TraySkippedFormat);
    const size_t at = msg.find(L"{n}");
    if (at != std::wstring::npos) msg.replace(at, 3, std::to_wstring(n));
    s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::StagingTray), std::move(msg), false);
}

// Release one tray batch into the current folder through the ops layer.
// Every paste that moves a cut finishes it the same way: once all of the cut's
// items have moved (app_main's completion handler), the system clipboard is
// emptied - but only while it still holds exactly that cut (sequence number
// and paths), so a newer copy is never touched.
static void TrackCutClipboard(AppState& s, const ops::ClipboardData& cut) {
    s.cutPaths.clear();
    s.pendingCutClipboardSequence = cut.sequence;
    s.pendingCutClipboardPaths = cut.paths;
    s.completedCutClipboardPaths.clear();
}

// Ctrl+X mirrors a tray move batch onto the system clipboard (CollectToTray).
// When the tray releases those items instead, the mirrored cut is finished as
// well, as long as every clipboard path is among the items being moved.
static void TrackTrayCutClipboard(AppState& s, const std::vector<std::wstring>& moved) {
    ops::ClipboardData cut;
    if (moved.empty() || !ops::ReadClipboard(cut) || !cut.cut) return;
    for (const auto& path : cut.paths) {
        const std::wstring normalized = fs::NormalizePath(path);
        const bool staged = std::any_of(moved.begin(), moved.end(), [&](const std::wstring& item) {
            return _wcsicmp(fs::NormalizePath(item).c_str(), normalized.c_str()) == 0;
        });
        if (!staged) return;
    }
    TrackCutClipboard(s, cut);
}

void ReleaseTrayBatch(AppState& s, size_t idx) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || idx >= s.tray.batches().size()) return;
    if (fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    const app::TrayBatch& b = s.tray.batches()[idx];
    ops::OpRequest req;
    req.type = b.move_intent ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = tab->current_path;
    std::vector<uint64_t> item_ids;
    size_t skipped = 0;
    for (const auto& it : b.items) {
        if (s.tray.IsInFlight(it.path)) continue;
        if (it.exists) {
            if (std::none_of(req.sources.begin(), req.sources.end(), [&](const auto& source) {
                return _wcsicmp(source.c_str(), it.path.c_str()) == 0;
            })) req.sources.push_back(it.path);
            item_ids.push_back(it.id);
        }
        else ++skipped;
    }
    NotifyTraySkipped(s, skipped);
    if (req.sources.empty()) return;
    const bool move = b.move_intent;
    const auto moved = move ? req.sources : std::vector<std::wstring>{};
    const auto task = s.ops.Submit(std::move(req));
    if (!task) return;
    if (move) TrackTrayCutClipboard(s, moved);
    s.tray.MarkInFlight(task, item_ids, move);
    RememberTrayDest(s, tab->current_path);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::vector<std::wstring> TrayDestList(const AppState& s) {
    std::vector<std::wstring> out;
    const std::wstring& all = s.appPrefs.tray_dests;
    size_t start = 0;
    while (start < all.size() && out.size() < 3) {
        size_t bar = all.find(L'|', start);
        if (bar == std::wstring::npos) bar = all.size();
        if (bar > start) out.push_back(all.substr(start, bar - start));
        start = bar + 1;
    }
    return out;
}

void RememberTrayDest(AppState& s, const std::wstring& dir) {
    if (dir.empty() || fs::IsVirtualPath(dir)) return;
    // Stored for display: no \\?\ long-path prefix.
    const std::wstring norm = ClipboardPath(fs::NormalizePath(dir));
    std::vector<std::wstring> list{ norm };
    for (const auto& old : TrayDestList(s)) {
        if (_wcsicmp(old.c_str(), norm.c_str()) != 0 && list.size() < 3) list.push_back(old);
    }
    std::wstring joined;
    for (const auto& d : list) {
        if (!joined.empty()) joined += L'|';
        joined += d;
    }
    if (joined == s.appPrefs.tray_dests) return;
    s.appPrefs.tray_dests = std::move(joined);
    s.appPrefs.Save();
}

void SendTrayToDest(AppState& s, const std::wstring& dir, bool move) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = fs::NormalizePath(dir);
    std::vector<uint64_t> item_ids;
    size_t skipped = 0;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) {
            if (s.tray.IsInFlight(item.path)) continue;
            if (GetFileAttributesW(item.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (std::none_of(req.sources.begin(), req.sources.end(), [&](const auto& source) {
                    return _wcsicmp(source.c_str(), item.path.c_str()) == 0;
                })) req.sources.push_back(item.path);
                item_ids.push_back(item.id);
            } else
                ++skipped;
        }
    NotifyTraySkipped(s, skipped);
    if (req.sources.empty()) return;
    const auto moved = move ? req.sources : std::vector<std::wstring>{};
    const auto task = s.ops.Submit(std::move(req));
    if (!task) return;
    if (move) TrackTrayCutClipboard(s, moved);
    s.tray.MarkInFlight(task, item_ids, move);
    RememberTrayDest(s, dir);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Ctrl+V: release the newest tray batch while it is still the latest copy,
// else paste from the system clipboard (paste_source.h).
void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    ops::ClipboardData cb;
    const bool clipboard_files = ops::ReadClipboard(cb);
    if (!s.tray.batches().empty()) {
        const size_t idx = s.tray.batches().size() - 1;
        const app::TrayBatch& newest = s.tray.batches()[idx];
        std::vector<std::wstring> batch_paths;
        batch_paths.reserve(newest.items.size());
        for (const auto& item : newest.items) batch_paths.push_back(item.path);
        app::PasteChoiceInput choice;
        choice.batch_paths = &batch_paths;
        choice.batch_move = newest.move_intent;
        choice.clipboard_has_files = clipboard_files;
        choice.clipboard_paths = &cb.paths;
        choice.clipboard_cut = cb.cut;
        choice.clipboard_written_for_batch = clipboard_files && s.trayClipboardSequence != 0 &&
            s.trayClipboardBatchId == newest.id && cb.sequence == s.trayClipboardSequence;
        if (app::ChoosePasteSource(choice) == app::PasteSource::TrayBatch) {
            if (newest.move_intent) s.cutPaths.clear();
            ReleaseTrayBatch(s, idx);
            return;
        }
    }
    if (clipboard_files) {
        ops::OpRequest req;
        req.type = cb.cut ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = tab->current_path;
        for (auto& p : cb.paths) req.sources.push_back(fs::NormalizePath(p));
        if (SubmitWithConflictResolution(s, std::move(req)) && cb.cut) TrackCutClipboard(s, cb);
    }
}

ui::ConfirmDialogSpec BuildRecycleDeleteConfirm(const std::vector<std::wstring>& paths) {
    ui::ConfirmDialogSpec confirm;
    confirm.title = l10n::Get(l10n::StringId::Delete);
    if (paths.size() == 1) {
        confirm.message = l10n::Get(l10n::StringId::RecycleConfirmOne);
    } else {
        wchar_t buf[256]{};
        swprintf_s(buf, l10n::Get(l10n::StringId::RecycleConfirmManyFormat).c_str(), paths.size());
        confirm.message = buf;
    }
    // The dialog shows the first few and collapses the rest.
    for (const std::wstring& path : paths) confirm.items.push_back(ClipboardPath(path));
    confirm.confirm_text = l10n::Get(l10n::StringId::Delete);
    return confirm;
}

void DeleteSelected(AppState& s, bool permanent) {
    if(DeferContentSelection(s,[=](AppState& v){DeleteSelected(v,permanent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (IsRecycleTab(tab)) {
        std::vector<std::wstring> paths;
        size_t item_count = 0;
        if (tab->snapshot) {
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
                const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(index));
                if (entry.recycle_path.empty()) continue;
                ++item_count;
                paths.push_back(entry.recycle_path);
                const std::wstring index_path = fs::RecycleIndexPath(entry.recycle_path);
                if (!index_path.empty()) paths.push_back(index_path);
            }
        }
        if (paths.empty()) return;
        ui::ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(l10n::StringId::PermanentDelete);
        wchar_t message[256]{};
        swprintf_s(message, l10n::Get(l10n::StringId::RecycleDeleteConfirmFormat).c_str(),
                   item_count);
        confirm.message = message;
        confirm.confirm_text = l10n::Get(l10n::StringId::PermanentDelete);
        confirm.danger = true;
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
        ops::OpRequest req;
        req.type = ops::OpType::RealDelete;
        req.sources = std::move(paths);
        s.ops.Submit(std::move(req));
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    DeletePaths(s, std::move(paths), permanent);
}

void DeletePaths(AppState& s, std::vector<std::wstring> paths, bool permanent) {
    if (paths.empty()) return;
    if (permanent) {
        ui::ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(l10n::StringId::PermanentDelete);
        if (paths.size() == 1) {
            confirm.message = l10n::Get(l10n::StringId::PermanentDeleteOne);
        } else {
            wchar_t buf[256]{};
            swprintf_s(buf, l10n::Get(l10n::StringId::PermanentDeleteManyFormat).c_str(),
                       paths.size());
            confirm.message = buf;
        }
        // The dialog shows the first few and collapses the rest.
        for (const std::wstring& path : paths) confirm.items.push_back(ClipboardPath(path));
        confirm.confirm_text = l10n::Get(l10n::StringId::PermanentDelete);
        confirm.danger = true;
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
    } else if (s.appPrefs.confirm_recycle_delete) {
        const ui::ConfirmDialogSpec confirm = BuildRecycleDeleteConfirm(paths);
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
    }
    ops::OpRequest req;
    req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

// ---------------------------------------------------------------------------
// Stage 1B-2: built-in Fluent context menu + new-item dropdown.
void RestoreSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const auto& path = tab->EntryAt(static_cast<size_t>(index)).recycle_path;
        if (!path.empty()) paths.push_back(path);
    }
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void RestoreAllRecycle(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths;
    const size_t count = tab->EntryCount();
    paths.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::wstring full = tab->EntryAt(i).recycle_path;
        if (!full.empty()) paths.push_back(std::move(full));
    }
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void EmptyRecycleBin(AppState& s) {
    ui::ConfirmDialogSpec confirm;
    confirm.title = l10n::Get(l10n::StringId::EmptyRecycleConfirmTitle);
    confirm.message = l10n::Get(l10n::StringId::EmptyRecycleConfirm);
    confirm.confirm_text = l10n::Get(l10n::StringId::EmptyRecycleBin);
    confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
    confirm.danger = true;
    if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
    ops::OpRequest req;
    req.type = ops::OpType::EmptyRecycle;
    s.ops.Submit(std::move(req));
}
void CollectToTray(AppState& s, bool move_intent) {
    if(DeferContentSelection(s,[=](AppState& v){CollectToTray(v,move_intent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    CollectPathsToTray(s, paths, move_intent);
}

void CollectPathsToTray(AppState& s, const std::vector<std::wstring>& paths, bool move_intent) {
    if (!paths.empty()) {
        s.tray.Collect(paths, move_intent);
        // Mirror the cut state onto the list rows (ui.md §5.2 rule 6).
        s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
        // Interop with Explorer: mirror the collection onto the system clipboard.
        std::vector<std::wstring> cbPaths;
        cbPaths.reserve(paths.size());
        for (const auto& p : paths) cbPaths.push_back(ClipboardPath(p));
        // Remember which clipboard value belongs to the new batch so Ctrl+V can
        // tell it from a later copy made elsewhere (paste_source.h).
        s.trayClipboardBatchId = s.tray.batches().empty() ? 0 : s.tray.batches().back().id;
        s.trayClipboardSequence =
            ops::WriteClipboard(cbPaths, move_intent) ? GetClipboardSequenceNumber() : 0;
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}
void PinAndShowOperationWindow(AppState& s) {
    if (!s.operationWindow) return;
    const ops::OpStatus status = s.ops.Status();
    if (!status.active && status.summary.empty() && status.last_error.empty()) return;
    s.operationDismissedTaskId = 0;
    s.operationPinnedByUser = true;
    s.operationAutoShown = true;
    s.operationWindow->Update(status);
    s.operationWindow->Show(true);
}

// Both batch-rename entries (the selection and the tray) submit through here.
// Nothing else changes up front: places and the tray follow the renames that
// actually succeeded once the operation completes (app_main's completion
// handler), so a failed or cancelled item never leaves a stale new name behind.
static void SubmitBatchRename(AppState& s, const std::vector<std::wstring>& paths, app::Tab* select_in) {
    if (paths.size() < 2) return;
    const auto result = ui::ShowBatchRenameDialog(s.hwnd, paths, s.darkMode, s.accentColor);
    if (!result.accepted) return;
    ops::OpRequest req;
    req.type = ops::OpType::BatchRename;
    for (const auto& item : result.items) {
        if (item.status != app::BatchRenameStatus::Ok) continue;
        req.sources.push_back(item.source_path);
        req.new_names.push_back(item.new_name);
    }
    if (req.sources.empty()) return;
    if (select_in) {
        select_in->pending_selected_names = req.new_names;
        select_in->pending_selected_name = req.new_names.front();
    }
    s.ops.Submit(std::move(req));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowBatchRename(AppState& s) {
    if(DeferContentSelection(s,[=](AppState& v){ShowBatchRename(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly) return;
    SubmitBatchRename(s, SelectedFullPaths(*tab), tab);
}
void ShowBatchRenamePaths(AppState& s, const std::vector<std::wstring>& paths) {
    SubmitBatchRename(s, paths, nullptr);
}

void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog) {
    if (!s.operationWindow) return;
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    s.operationWindow->Update(status);

    if (status.active && status.task_id != s.operationUiTaskId) {
        s.operationUiTaskId = status.task_id;
        s.operationAutoShown = false;
        s.operationPinnedByUser = false;
        s.operationStartedAt = now;
        s.operationFinishedAt = {};
    }

    if (allow_conflict_dialog) {
        if (const auto conflict = s.ops.PendingConflict();
            conflict && conflict->token != s.conflictUiToken) {
            s.conflictUiToken = conflict->token;
            const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                s.hwnd, *conflict, s.darkMode, s.accentColor);
            s.ops.ResolveConflict(conflict->token, result.choice, result.apply_to_all);
        }
    }

    // Failed on an item other processes hold: name them and offer the retry
    // instead of the plain failure window (the token guards modal re-entry).
    const bool locked_failure = !status.active && status.phase == ops::OpPhase::Failed
        && !status.lock_owners.empty() && status.task_id != 0;
    if (locked_failure && allow_conflict_dialog && status.task_id != s.lockPromptTaskId) {
        s.lockPromptTaskId = status.task_id;
        s.operationDismissedTaskId = status.task_id;
        s.operationPinnedByUser = false;
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        PromptLockedItem(s, status);
        return;
    }

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId &&
        !s.operationPinnedByUser) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (status.active && status.authorization != ops::AuthorizationState::None) {
        // Keep the existing operation window visible without stealing focus from UAC.
        if (!s.operationWindow->IsVisible()) s.operationWindow->Show(false);
        return;
    }

    if (status.active) {
        // Progress lives in the status-bar pill; the details window opens only
        // when the pill is clicked (conflicts and failures still surface).
        return;
    }

    if (s.operationPinnedByUser) return;

    if (status.phase == ops::OpPhase::Completed) {
        if (s.operationWindow->IsVisible()) {
            if (s.operationFinishedAt.time_since_epoch().count() == 0)
                s.operationFinishedAt = now;
            if (now - s.operationFinishedAt >= std::chrono::milliseconds(600))
                s.operationWindow->Hide();
        }
    } else if (status.phase == ops::OpPhase::Failed) {
        // The ops notification shows the locked-item prompt; a timer tick
        // that sees the failure first must not open the failure window.
        if (locked_failure) return;
        if (ErrorIs(status.last_error, L"已取消", L"Canceled")) {
            s.operationWindow->Hide();
            return;
        }
        const bool simple = status.type == ops::OpType::CreateFolder
                         || status.type == ops::OpType::CreateTextFile
                         || status.type == ops::OpType::Rename
                         || status.type == ops::OpType::BatchRename;
        if (simple) {
            if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
            if (status.task_id == 0 || status.task_id == s.operationDismissedTaskId)
                return;
            s.operationDismissedTaskId = status.task_id;
            const bool folder = status.type == ops::OpType::CreateFolder;
            const bool file = status.type == ops::OpType::CreateTextFile;
            const std::wstring title = l10n::Get(folder ? l10n::StringId::CannotCreateFolder
                : file ? l10n::StringId::CannotCreateTextFile : l10n::StringId::CannotRename);
            std::wstring message;
            const std::wstring& err = status.last_error;
            const bool no_access = ErrorHas(err, L"没有权限")
                || ErrorHas(err, L"拒绝访问")
                || err.find(L"存取被拒") != std::wstring::npos  // zh-TW system message
                || err.find(L"Access is denied") != std::wstring::npos
                || err == L"create failed";
            if (no_access) {
                message = l10n::Get(folder || file
                    ? l10n::StringId::FolderNoWritePermission
                    : l10n::StringId::RenameNoPermission);
            } else if (ErrorIs(err, L"目标名称已存在", L"The target name already exists")) {
                message = l10n::Get(l10n::StringId::RenameTargetExists);
            } else if (ErrorIs(err, L"名称无效", L"Invalid name")) {
                message = l10n::Get(l10n::StringId::InvalidName);
            } else if (!err.empty()) {
                // pulse_shell reports Simplified text; known messages are localized,
                // file names and unknown messages are shown as reported.
                message = l10n::ServiceErrorText(err);
            } else {
                message = l10n::Get(l10n::StringId::OperationFailedMessage);
            }
            s.notification_toast.ShowError(s.hwnd, title, std::move(message));
            return;
        }
        if (!s.operationWindow->IsVisible()) s.operationWindow->Show(true);
    }
}

} // namespace pulse
