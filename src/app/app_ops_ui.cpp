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
bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request) {
    // Copy/move conflict discovery is part of the transfer worker's recursive
    // scan. The UI only consumes immutable conflict snapshots.
    s.ops.Submit(std::move(request));
    return true;
}

// Release one tray batch into the current folder through the ops layer.
void ReleaseTrayBatch(AppState& s, size_t idx) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || idx >= s.tray.batches().size()) return;
    if (fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    const app::TrayBatch& b = s.tray.batches()[idx];
    ops::OpRequest req;
    req.type = b.move_intent ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = tab->current_path;
    for (const auto& it : b.items) {
        if (it.exists) req.sources.push_back(it.path);
    }
    if (req.sources.empty()) return;
    if (!SubmitWithConflictResolution(s, std::move(req))) return;
    // Cut batches are consumed by release; copy batches too (default per ui.md §7.4).
    s.tray.RemoveBatch(idx);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Ctrl+V: release newest tray batch, else paste from the system clipboard.
void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    if (!s.tray.batches().empty()) {
        size_t idx = s.tray.batches().size() - 1;
        if (s.tray.batches()[idx].move_intent) s.cutPaths.clear();
        ReleaseTrayBatch(s, idx);
        return;
    }
    ops::ClipboardData cb;
    if (ops::ReadClipboard(cb)) {
        ops::OpRequest req;
        req.type = cb.cut ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = tab->current_path;
        for (auto& p : cb.paths) req.sources.push_back(fs::NormalizePath(p));
        if (SubmitWithConflictResolution(s, std::move(req)) && cb.cut) {
            s.cutPaths.clear();
            s.pendingCutClipboardSequence = cb.sequence;
            s.pendingCutClipboardPaths = cb.paths;
            s.completedCutClipboardPaths.clear();
        }
    }
}

void DeleteSelected(AppState& s, bool permanent) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (IsRecycleTab(tab)) {
        std::vector<std::wstring> paths;
        if (tab->snapshot) {
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->snapshot->size())) continue;
                const fs::DirEntry& entry = (*tab->snapshot)[static_cast<size_t>(index)];
                if (entry.recycle_path.empty()) continue;
                paths.push_back(entry.recycle_path);
                const std::wstring index_path = fs::RecycleIndexPath(entry.recycle_path);
                if (!index_path.empty()) paths.push_back(index_path);
            }
        }
        if (paths.empty()) return;
        std::wstring prompt = l10n::Get(l10n::StringId::PermanentDelete);
        prompt += L"\n\n";
        prompt += l10n::Get(l10n::StringId::EmptyRecycleConfirm);
        if (MessageBoxW(s.hwnd, prompt.c_str(), L"Pulse", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        ops::OpRequest req;
        req.type = ops::OpType::RealDelete;
        req.sources = std::move(paths);
        s.ops.Submit(std::move(req));
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    if (permanent) {
        std::wstring prompt;
        if (paths.size() == 1) {
            prompt = L"永久删除（不进回收站）：\n" + ClipboardPath(paths[0]) + L"\n\n确定吗？";
        } else {
            wchar_t buf[96];
            swprintf_s(buf, L"永久删除（不进回收站）%d 项？\n\n确定吗？", static_cast<int>(paths.size()));
            prompt = buf;
        }
        if (MessageBoxW(s.hwnd, prompt.c_str(), L"Pulse", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
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
    if (!tab) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
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
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (!paths.empty()) {
        s.tray.Collect(paths, move_intent);
        // Mirror the cut state onto the list rows (ui.md §5.2 rule 6).
        s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
        // Interop with Explorer: mirror the collection onto the system clipboard.
        std::vector<std::wstring> cbPaths;
        cbPaths.reserve(paths.size());
        for (const auto& p : paths) cbPaths.push_back(ClipboardPath(p));
        ops::WriteClipboard(cbPaths, move_intent);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}
void ShowBatchRename(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
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
    tab->pending_selected_names = req.new_names;
    if (!req.new_names.empty()) tab->pending_selected_name = req.new_names.front();
    s.ops.Submit(std::move(req));
}
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog) {
    if (!s.operationWindow) return;
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    s.operationWindow->Update(status);

    if (status.active && status.task_id != s.operationUiTaskId) {
        s.operationUiTaskId = status.task_id;
        s.operationAutoShown = false;
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

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (status.active) {
        if (status.phase == ops::OpPhase::WaitingForConflict) return;
        // Operations that finish quickly (e.g. deleting an empty folder) never
        // surface a window; only long-running work gets the progress dialog.
        if (!s.operationAutoShown &&
            now - s.operationStartedAt >= std::chrono::milliseconds(2000)) {
            s.operationWindow->Show(false);
            s.operationAutoShown = true;
        }
        return;
    }

    if (status.phase == ops::OpPhase::Completed) {
        if (s.operationWindow->IsVisible()) {
            if (s.operationFinishedAt.time_since_epoch().count() == 0)
                s.operationFinishedAt = now;
            if (now - s.operationFinishedAt >= std::chrono::milliseconds(600))
                s.operationWindow->Hide();
        }
    } else if (status.phase == ops::OpPhase::Failed) {
        if (status.last_error == L"已取消") {
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
            if (status.type == ops::OpType::CreateFolder ||
                status.type == ops::OpType::CreateTextFile)
                s.pendingRenameName.clear();
            const bool folder = status.type == ops::OpType::CreateFolder;
            const bool file = status.type == ops::OpType::CreateTextFile;
            std::wstring text = folder ? L"无法新建文件夹"
                : file ? L"无法新建文本文档" : L"无法重命名";
            text += L"\n\n";
            const std::wstring& err = status.last_error;
            const bool no_access = err.find(L"没有权限") != std::wstring::npos
                || err.find(L"拒绝访问") != std::wstring::npos
                || err.find(L"Access is denied") != std::wstring::npos
                || err == L"create failed";
            if (no_access) {
                text += folder || file
                    ? L"当前文件夹没有写入权限。"
                    : L"没有权限重命名此项。";
            } else if (!err.empty()) {
                text += err;
            } else {
                text += L"操作失败。";
            }
            MessageBoxW(s.hwnd, text.c_str(), L"Pulse", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!s.operationWindow->IsVisible()) s.operationWindow->Show(true);
    }
}

} // namespace pulse
