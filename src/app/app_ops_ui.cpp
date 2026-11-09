#include "delete_departure.h"
#include "delete_preflight.h"
// app_ops_ui.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "app_messages.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/ui_renderer.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/ai_list_overlay.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "../common/runtime_log.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "ai_assistant_chat.h"
#include "ai_model_key.h"
#include "ai_model_stream.h"
#include "ai_providers.h"
#include "link_resolve.h"
#include "locked_item_prompt.h"
#include "selection_caps.h"
#include "app_commands.h"
#include "reopen_closed.h"
#include "paste_source.h"
#include "arrival_select.h"
#include "tray_reveal.h"
#include "resource.h"
#include "../index/document_reader.h"
#include "../ops/clipboard.h"
#include "../ops/elevated_delete.h"
#include "../ipc/ctx_menu_util.h"
#include "device_registry.h"
#include "dup_finder.h"
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
#include <unordered_map>
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
    ArmArrivalSelection(s, nullptr, task, 0);
    if (move) TrackTrayCutClipboard(s, moved);
    s.tray.MarkInFlight(task, item_ids, move);
    RememberTrayDest(s, tab->current_path);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// The shelf's 送到 chips and ▾ menu remember the last five targets.
constexpr size_t kTrayDestMax = 5;

std::vector<std::wstring> TrayDestList(const AppState& s) {
    std::vector<std::wstring> out;
    const std::wstring& all = s.appPrefs.tray_dests;
    size_t start = 0;
    while (start < all.size() && out.size() < kTrayDestMax) {
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
        if (_wcsicmp(old.c_str(), norm.c_str()) != 0 && list.size() < kTrayDestMax) list.push_back(old);
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
    ReleaseTrayTo(s, dir, move);
}

static std::wstring FillText(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, wcslen(key), value);
    return text;
}

static std::wstring FolderLabel(const std::wstring& dir) {
    std::wstring d = dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    const size_t slash = d.find_last_of(L"\\/");
    std::wstring leaf = slash == std::wstring::npos ? d : d.substr(slash + 1);
    return leaf.empty() ? ClipboardPath(d) : leaf;
}

void ReleaseTrayTo(AppState& s, const std::wstring& dir_in, bool move) {
    if (dir_in.empty() || fs::IsVirtualPath(dir_in)) return;
    const std::wstring dir = fs::NormalizePath(dir_in);
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = dir;
    // Same names keep both ("name (2).ext"), like the shelf's 同名 badge promises.
    req.collision_policy = ops::CollisionPolicy::KeepBoth;
    std::vector<uint64_t> item_ids;
    size_t skipped = 0, already = 0;
    // Open view with checks: only the checked items go.
    const std::vector<uint64_t>* only = s.trayView.open && !s.trayView.checked.empty() ? &s.trayView.checked : nullptr;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) {
            if (only && std::find(only->begin(), only->end(), item.id) == only->end()) continue;
            if (s.tray.IsInFlight(item.path)) continue;
            if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                ++skipped;
                continue;
            }
            // Already there: nothing to copy, and a move would be a no-op.
            if (_wcsicmp(fs::NormalizePath(fs::ParentPath(item.path)).c_str(), dir.c_str()) == 0) {
                ++already;
                continue;
            }
            if (std::none_of(req.sources.begin(), req.sources.end(), [&](const auto& source) {
                return _wcsicmp(source.c_str(), item.path.c_str()) == 0;
            })) req.sources.push_back(item.path);
            item_ids.push_back(item.id);
        }
    NotifyTraySkipped(s, skipped);
    if (req.sources.empty()) {
        if (already && s.hwnd)
            s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
                FillText(l10n::Get(l10n::StringId::TrayToastAlreadyThere), L"{name}", FolderLabel(dir)), {}, {},
                {}, 3000);
        return;
    }
    const size_t count = req.sources.size();
    const auto moved = move ? req.sources : std::vector<std::wstring>{};
    const auto task = s.ops.Submit(std::move(req));
    if (!task) return;
    if (app::Tab* tab = ActiveTab(s);
        tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), dir.c_str()) == 0)
        ArmArrivalSelection(s, nullptr, task, 0);
    if (move) TrackTrayCutClipboard(s, moved);
    s.tray.MarkInFlight(task, item_ids, move);
    RememberTrayDest(s, dir);
    s.trayRelease = {task, count, move, dir};
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NoteTrayReleaseFinished(AppState& s, const ops::FinishedTask& done) {
    if (!s.trayRelease.task || done.task_id != s.trayRelease.task) return;
    const AppState::TrayReleaseNote note = std::move(s.trayRelease);
    s.trayRelease = {};
    if (!done.success || !s.hwnd) return;
    std::wstring title = l10n::Get(note.move ? l10n::StringId::TrayToastMoved : l10n::StringId::TrayToastCopied);
    title = FillText(std::move(title), L"{n}", std::to_wstring(note.count));
    title = FillText(std::move(title), L"{name}", FolderLabel(note.dest));
    std::vector<ui::SnackAction> actions;
    if (s.ops.CanUndo()) actions.push_back({l10n::Get(l10n::StringId::SnackUndo), WM_TRAY_UNDO});
    const app::Tab* tab = ActiveTab(s);
    const bool here = tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), note.dest.c_str()) == 0;
    if (!here) actions.push_back({l10n::Get(l10n::StringId::Open), WM_TRAY_OPEN_DEST});
    s.trayReleaseOpen = note.dest;
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, std::move(title), {}, std::move(actions),
                                   s.ops.CanUndo() ? L"Ctrl+Z" : L"");
}

std::vector<std::wstring> TrayLivePaths(const AppState& s) {
    std::vector<std::wstring> paths;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) {
            if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (std::none_of(paths.begin(), paths.end(), [&](const std::wstring& p) {
                    return _wcsicmp(p.c_str(), item.path.c_str()) == 0; }))
                paths.push_back(item.path);
        }
    return paths;
}

void ClearTrayWithUndo(AppState& s) {
    if (s.tray.batches().empty()) return;
    std::wstring saved;
    s.tray.ToJson(saved);
    s.trayClearedJson = std::move(saved);
    s.tray.Clear();
    if (s.hwnd) {
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, l10n::Get(l10n::StringId::TrayToastCleared), {},
            {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_TRAY_RESTORE}});
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

void RemoveTrayItems(AppState& s, const std::vector<uint64_t>& ids) {
    if (ids.empty()) return;
    // Re-scan after each removal: an emptied batch leaves and shifts the rest.
    for (bool found = true; found;) {
        found = false;
        const auto& batches = s.tray.batches();
        for (size_t b = 0; b < batches.size() && !found; ++b)
            for (size_t i = 0; i < batches[b].items.size(); ++i)
                if (std::find(ids.begin(), ids.end(), batches[b].items[i].id) != ids.end()) {
                    s.tray.RemoveItem(b, i);
                    found = true;
                    break;
                }
    }
    auto& checked = s.trayView.checked;
    checked.erase(std::remove_if(checked.begin(), checked.end(), [&](uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end(); }), checked.end());
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void UnstagePath(AppState& s, const std::wstring& path) {
    const std::wstring key = fs::NormalizePath(path);
    const auto& batches = s.tray.batches();
    for (size_t b = batches.size(); b-- > 0;) {
        const auto& items = s.tray.batches()[b].items;
        for (size_t i = items.size(); i-- > 0;) {
            if (_wcsicmp(fs::NormalizePath(items[i].path).c_str(), key.c_str()) != 0) continue;
            s.tray.RemoveItem(b, i);
            break;
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HandleTrayMessage(AppState& s, UINT message) {
    if (message == WM_TRAY_UNDO) {
        if (s.ops.CanUndo()) s.ops.Undo();
    } else if (message == WM_TRAY_OPEN_DEST) {
        if (!s.trayReleaseOpen.empty()) NavigateTo(s, s.trayReleaseOpen);
    } else if (message == WM_TRAY_TAKE_CLIP) {
        TakeTrayClip(s);
    } else if (message == WM_TRAY_RESTORE) {
        if (!s.trayClearedJson.empty() && s.tray.batches().empty()) s.tray.FromJson(s.trayClearedJson);
        s.trayClearedJson.clear();
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

// Ctrl+V: the system clipboard. The tray is released from the shelf only;
// Ctrl+C no longer stages.
void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (IsDeviceTab(tab)) {
        // A phone folder: files or another phone's items go in as a Pulse
        // task (Explorer's engine, Pulse's progress and conflict dialog).
        auto& devices = app::DeviceRegistry::Instance();
        if (!devices.FolderWritable(tab->current_path) || !ops::ClipboardHasPasteableFiles()) return;
        ops::ClipboardData files;
        std::vector<std::wstring> shell_ids;
        bool shell_cut = false;
        if (ops::ReadClipboard(files)) {
            const bool submitted = SubmitDeviceTransfer(s, files.paths, tab->current_path, files.cut);
            pulse::diagnostics::runtime::Event("device_paste", {{"route", 1}, {"submitted", submitted},
                {"items", files.paths.size()}});
            if (submitted) {
                if (files.cut) TrackCutClipboard(s, files);  // as a local paste: the cut ends with the move
                return;
            }
        } else if (ops::ReadClipboardShellItems(shell_ids, shell_cut)) {
            const size_t count = shell_ids.size();
            const bool submitted = SubmitDeviceTransfer(s, std::move(shell_ids), tab->current_path, shell_cut);
            pulse::diagnostics::runtime::Event("device_paste", {{"route", 2}, {"submitted", submitted},
                {"items", count}});
            if (submitted) return;
        }
        pulse::diagnostics::runtime::Event("device_paste", {{"route", 3}});
        // Anything else (a zip's files, an attachment): the shell's own paste.
        const std::wstring folder_id = devices.FolderShellId(tab->current_path);
        if (!folder_id.empty()) RunDeviceVerb(s, {folder_id}, L"paste", true);
        return;
    }
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly ||
        app::IsUncServerRoot(tab->current_path))
        return;
    ops::ClipboardData cb;
    const bool clipboard_files = ops::ReadClipboard(cb);
    // Virtual files (Remote Desktop, a zip, Outlook) are a newer copy too and
    // beat the tray batch like any other clipboard files (#109).
    const bool shell_files = !clipboard_files && ops::ClipboardHasShellFiles();
    std::vector<std::wstring> shell_ids;
    bool shell_cut = false;
    if (shell_files && ops::ReadClipboardShellItems(shell_ids, shell_cut)) {
        // A phone's items: a Pulse task like any paste, selected on arrival.
        uint64_t task = 0;
        if (SubmitDeviceTransfer(s, std::move(shell_ids), tab->current_path, shell_cut, &task)) {
            ArmArrivalSelection(s, nullptr, task, 0);
            return;
        }
    }
    if (clipboard_files) {
        ops::OpRequest req;
        req.type = cb.cut ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = tab->current_path;
        for (auto& p : cb.paths) req.sources.push_back(fs::NormalizePath(p));
        uint64_t task = 0;
        if (SubmitWithConflictResolution(s, std::move(req), &task)) {
            ArmArrivalSelection(s, nullptr, task, 0);
            if (cb.cut) TrackCutClipboard(s, cb);
        }
    } else if (shell_files) {
        // Virtual files (a zip's, an attachment): the shell writes them with
        // no task to follow. The shell host runs the drop so a long copy
        // never holds this thread; in process only if it is not there.
        ArmArrivalSelection(s, nullptr, 0, 8000);
        if (!s.ops.RunShellVerb({ClipboardPath(tab->current_path)}, L"paste", true, s.hwnd))
            ops::PasteShellClipboard(tab->current_path);
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

static void DeleteDeviceSelection(AppState& s, const app::Tab& tab);

void DeleteSelected(AppState& s, bool permanent) {
    if(DeferContentSelection(s,[=](AppState& v){DeleteSelected(v,permanent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (app::RecycleSelectionOf(*tab) == app::RecycleSelection::Mixed) return;
    if (IsRecycleTab(tab) || app::RecycleSelectionOf(*tab) == app::RecycleSelection::All) {
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
    // Network view: Del removes the selected pins, never the shares behind
    // them. Drives and a server's shares are not deleted at all.
    const app::SelectionCaps caps = SelectionCapsOf(s);
    if (caps.kind == app::SelectionKind::NetworkPlaces) {
        UnpinSelectedNetworks(s);
        return;
    }
    if (!caps.del) {
        // A phone's rows the shell grants neither delete nor rename.
        if (caps.kind == app::SelectionKind::Device)
            pulse::diagnostics::runtime::Event("device_delete_blocked", {{"read_only", 1},
                {"selected", static_cast<uint64_t>(tab->SelectedCount())}});
        return;
    }
    if (caps.kind == app::SelectionKind::Device) {
        DeleteDeviceSelection(s, *tab);
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    DeletePaths(s, std::move(paths), permanent);
}

namespace {
// `no_recycle_bin`: the items were meant for the Recycle Bin but live where
// there is none, so say why the delete is permanent (Explorer asks the same).
// `offer_recycle`: Shift+Delete on items that do have a bin; the dialog then
// offers 移到回收站 instead.
ui::PermanentDeleteChoice ConfirmPermanentDelete(AppState& s, const std::vector<std::wstring>& paths,
                                                 bool no_recycle_bin, bool offer_recycle = false,
                                                 ui::PermanentDeleteInfo::NoBinReason reason =
                                                     ui::PermanentDeleteInfo::NoBinReason::Location) {
    ui::PermanentDeleteInfo info;
    info.paths.reserve(paths.size());
    for (const std::wstring& path : paths) info.paths.push_back(ClipboardPath(path));
    info.no_recycle_bin = no_recycle_bin;
    info.no_bin_reason = reason;
    info.offer_recycle = offer_recycle;
    return ui::ShowPermanentDeleteDialog(s.hwnd, info, s.darkMode, s.accentColor);
}


}

// A phone has no Recycle Bin: Pulse asks as Explorer does ("permanently
// delete?", this location has no Recycle Bin), then Explorer's engine deletes.
static void DeleteDeviceSelection(AppState& s, const app::Tab& tab) {
    std::vector<std::wstring> ids = SelectedShellIds(tab);
    if (ids.empty()) {
        // A selected row without its shell id (not listed by the shell).
        pulse::diagnostics::runtime::Event("device_delete_blocked", {{"missing_id", 1},
            {"selected", static_cast<uint64_t>(tab.SelectedCount())}});
        return;
    }
    const std::vector<std::wstring> rows = SelectedFullPaths(tab);
    std::vector<std::wstring> shown;
    shown.reserve(rows.size());
    for (const auto& path : rows) shown.push_back(DeviceFriendlyPath(path));
    const ui::PermanentDeleteChoice choice =
        ConfirmPermanentDelete(s, shown, true, false, ui::PermanentDeleteInfo::NoBinReason::Location);
    pulse::diagnostics::runtime::Event("device_delete_confirm", {{"choice", static_cast<uint64_t>(choice)},
        {"selected", ids.size()}});
    if (choice != ui::PermanentDeleteChoice::Delete) return;
    ops::OpRequest req;
    req.type = ops::OpType::RealDelete;
    req.sources = std::move(ids);
    req.refresh_directories.push_back(tab.current_path);
    s.ops.Submit(std::move(req));
}

void NoteDeleteCompletion(AppState& s, const ops::CompletedOperation& completed) {
    if (app::RecordConfirmedDelete(completed, s.leavingPaths, s.leavingSince, s.leavingDoneAt,
        s.leavingTask, ui::motion::NowMs()) && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NoteDeleteFinished(AppState& s, const ops::FinishedTask& done) {
    if (done.type != ops::OpType::RecycleDelete && done.type != ops::OpType::RealDelete) {
        // Anything else (the undo itself, a later copy...) ends the slide-out
        // hold, and the snack's Undo would no longer undo the delete.
        s.leavingPaths.clear();
        DismissReopenToasts(s, WM_UNDO_DELETE);
        return;
    }
    if (done.task_id == s.leavingTask && !s.leavingPaths.empty()) {
        if (done.success) s.leavingDoneAt = ui::motion::NowMs();
        else s.leavingPaths.clear();
    }
    if (done.type == ops::OpType::RecycleDelete && !done.recycle_refused.empty() && s.hwnd) {
        // Asked from a posted message: this runs inside the ops notification.
        const bool first = s.recycleRefused.empty();
        s.recycleRefused.insert(s.recycleRefused.end(), done.recycle_refused.begin(), done.recycle_refused.end());
        using Refusal = ops::RecycleRefusal;
        const Refusal why = done.recycle_refusal;
        if (why == Refusal::Location || s.recycleRefusal == Refusal::None ||
            (why == Refusal::Disabled && s.recycleRefusal == Refusal::TooLarge))
            s.recycleRefusal = why;
        if (first) PostMessageW(s.hwnd, WM_RECYCLE_REFUSED, 0, 0);
    }
    // A finished recycle no longer raises an Undo capsule: the status bar's
    // undo chip names the step and takes it back (Ctrl+Z still works).
}

void CancelDeletePreflight(AppState& s) {
    if (s.delete_preflight) {
        s.delete_preflight->cancel.store(true);
        s.delete_preflight->cancelled = true;
        s.delete_preflight->terminal = s.delete_preflight->task_id == 0;
        if (s.delete_preflight->confirmation)
            PostMessageW(s.delete_preflight->confirmation, WM_CLOSE, 0, 0);
    }
    s.delete_preflight.reset();
    KillTimer(s.hwnd, 0xD311);
    s.delete_preflight_window.Destroy();
}

std::shared_ptr<DeletePreflight> DeletePaths(AppState& s, std::vector<std::wstring> paths, bool permanent,
    bool recycle_only, bool force_confirmation) {
    if (paths.empty() || (recycle_only && s.delete_preflight)) return {};
    CancelDeletePreflight(s);
    const auto show_failure = [&s] {
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
            l10n::Get(l10n::StringId::Delete), l10n::Get(l10n::StringId::OperationFailedMessage), {}, {}, 5000);
    };
    auto request = std::make_shared<DeletePreflight>();
    request->notify_window = s.hwnd;
    request->paths = std::move(paths);
    request->permanent = permanent;
    request->recycle_only = recycle_only;
    request->force_confirmation = force_confirmation;
    if (s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) {
        wchar_t delay_text[32]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_DELAY_MS", delay_text, 32);
        const int delay_ms = length > 0 && length < 32 ? std::clamp(_wtoi(delay_text), 0, 30000) : 0;
        if (delay_ms) {
            const std::weak_ptr<DeletePreflight> weak = request;
            request->before_probe = [weak, delay_ms] {
                const auto pending = weak.lock();
                for (int elapsed = 0; pending && elapsed < delay_ms && !pending->cancel.load(); elapsed += 10)
                    Sleep(static_cast<DWORD>(std::min(10, delay_ms - elapsed)));
            };
        }
    }
    s.delete_preflight = request;
    StartDeletePreflight(request);
    if (!SetTimer(s.hwnd, 0xD311, 30, nullptr)) {
        request->dispatch_failed = true;
        CancelDeletePreflight(s);
        request->terminal = true;
        show_failure();
    }
    return request;
}

void PollDeletePreflight(AppState& s) {
    const auto request = s.delete_preflight;
    if (!request) return;
    if (request->cancel.load()) { CancelDeletePreflight(s); return; }
    if (!request->done.load()) {
        if (!ShouldShowDeletePreparation(*request, GetTickCount64())) return;
        request->preparation_shown = true;
        ui::FileOperationCallbacks callbacks;
        callbacks.cancel = [request] { request->cancel.store(true); };
        callbacks.dismiss = callbacks.cancel;
        if (!s.delete_preflight_window.Create(s.hwnd, std::move(callbacks))) {
            request->dispatch_failed = true;
            CancelDeletePreflight(s);
            s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
                l10n::Get(l10n::StringId::Delete), l10n::Get(l10n::StringId::OperationFailedMessage), {}, {}, 5000);
            return;
        }
        s.delete_preflight_window.SetPreparationOnly(true);
        s.delete_preflight_window.SetTheme(s.darkMode, s.accentColor);
        ops::OpStatus status;
        status.active = true;
        status.can_pause = false;
        status.type = request->permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
        status.phase = ops::OpPhase::Scanning;
        status.enumeration_complete = true;
        status.total_items = request->input_count;
        status.source_label = ClipboardPath(request->first_path);
        s.delete_preflight_window.Update(status);
        s.delete_preflight_window.Show(false);
        return;
    }
    struct CompletionCleanup {
        HWND owner;
        std::shared_ptr<DeletePreflight> request;
        ~CompletionCleanup() {
            if (auto* state = GetAppState(owner); state && state->delete_preflight == request)
                state->delete_preflight.reset();
        }
    } cleanup{s.hwnd, request};
    const HWND owner = s.hwnd;
    const AppState* expected_state = &s;
    const auto still_current = [owner, expected_state, request] {
        const auto* state = GetAppState(owner);
        return state && state == expected_state && CanSubmitDeletePreflight(state->delete_preflight, request);
    };
    KillTimer(s.hwnd, 0xD311);
    s.delete_preflight_window.Destroy();
    if (request->cancel.load()) return;
    if (request->failed) {
        request->terminal = true;
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
            l10n::Get(l10n::StringId::Delete), l10n::Get(l10n::StringId::OperationFailedMessage), {}, {}, 5000);
        return;
    }
    if (request->recycle_only && !request->direct.empty()) {
        request->failed = true;
        request->terminal = true;
        return;
    }
    auto paths = std::move(request->paths);
    auto direct = std::move(request->direct);
    bool permanent = request->permanent;
    const auto reason = request->reason;
    if (!permanent) {
        if (!direct.empty()) {
            if (ConfirmPermanentDelete(s, direct, true, false, reason) != ui::PermanentDeleteChoice::Delete || !still_current()) return;
            ops::OpRequest req;
            req.type = ops::OpType::RealDelete;
            req.sources = std::move(direct);
            s.ops.Submit(std::move(req));
            if (paths.empty()) return;
        }
    }
    if (permanent) {
        const auto choice = ConfirmPermanentDelete(s, paths, false, request->offer_recycle);
        if (choice == ui::PermanentDeleteChoice::Cancel || !still_current()) return;
        // 移到回收站 from the dialog: the user already confirmed, don't ask again.
        if (choice == ui::PermanentDeleteChoice::Recycle) permanent = false;
    } else if (request->force_confirmation || s.appPrefs.confirm_recycle_delete) {
        ui::ConfirmDialogSpec confirm = BuildRecycleDeleteConfirm(paths);
        if (request->force_confirmation) confirm.default_choice = ui::ConfirmChoice::Cancel;
        request->waiting_confirmation = true;
        std::weak_ptr<DeletePreflight> weak = request;
        const bool isolated = s.isolatedTest;
        confirm.on_open = [weak, isolated](HWND window) {
            if (const auto value = weak.lock()) {
                value->confirmation = window;
                if (isolated && value->confirmation_observer) value->confirmation_observer(window);
            }
        };
        const bool accepted = ui::ShowConfirmDialogEx(s.hwnd, confirm, s.darkMode, s.accentColor) == ui::ConfirmChoice::Confirm;
        request->waiting_confirmation = false;
        request->confirmation = nullptr;
        if (!accepted || !still_current()) {
            request->cancelled = true;
            request->terminal = true;
            return;
        }
    }
    if (!still_current()) { request->cancelled = true; request->terminal = true; return; }
    ops::OpRequest req;
    req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    request->task_id = s.ops.Submit(std::move(req));
    if (request->task_id && request->recycle_only && !s.ops.RetainTask(request->task_id)) {
        s.ops.CancelTask(request->task_id);
        request->failed = true;
    }
    if (!request->task_id) {
        request->failed = true;
        request->terminal = true;
    }
}

void OfferPermanentDeleteForRefused(AppState& s) {
    std::vector<std::wstring> paths = std::move(s.recycleRefused);
    const ops::RecycleRefusal refusal = s.recycleRefusal;
    s.recycleRefused.clear();
    s.recycleRefusal = ops::RecycleRefusal::None;
    // What is still there (another task may have dealt with some meanwhile);
    // a huge list is left to the delete to report.
    if (paths.size() <= 256)
        std::erase_if(paths, [](const std::wstring& path) {
            return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
        });
    if (paths.empty()) return;
    using Reason = ui::PermanentDeleteInfo::NoBinReason;
    const Reason reason = refusal == ops::RecycleRefusal::Location ? Reason::Location
        : refusal == ops::RecycleRefusal::Disabled ? Reason::Disabled : Reason::TooLarge;
    if (ConfirmPermanentDelete(s, paths, true, false, reason) != ui::PermanentDeleteChoice::Delete) return;
    ops::OpRequest req;
    req.type = ops::OpType::RealDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

// ---------------------------------------------------------------------------
// Stage 1B-2: built-in Fluent context menu + new-item dropdown.
void RestoreSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || app::RecycleSelectionOf(*tab) != app::RecycleSelection::All) return;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> parents;
    std::unordered_set<std::wstring> parent_keys;
    std::wstring first_name; // the status names the item, not its $R payload
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const auto entry = tab->EntryAt(static_cast<size_t>(index));
        if (entry.recycle_path.empty()) continue;
        if (paths.empty()) first_name = entry.name;
        paths.push_back(entry.recycle_path);
        const auto parent = fs::ParentPath(entry.full_path);
        if (!parent.empty() && parent_keys.insert(path::FoldedKey(parent)).second)
            parents.push_back(parent);
    }
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.refresh_directories = std::move(parents);
    req.sources = std::move(paths);
    req.display_name = std::move(first_name);
    s.ops.Submit(std::move(req));
}

void RestoreAllRecycle(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> parents;
    std::unordered_set<std::wstring> parent_keys;
    const size_t count = tab->EntryCount();
    paths.reserve(count);
    std::wstring first_name;
    for (size_t i = 0; i < count; ++i) {
        const auto entry = tab->EntryAt(i);
        if (entry.recycle_path.empty()) continue;
        if (paths.empty()) first_name = entry.name;
        paths.push_back(entry.recycle_path);
        const auto parent = fs::ParentPath(entry.full_path);
        if (!parent.empty() && parent_keys.insert(path::FoldedKey(parent)).second)
            parents.push_back(parent);
    }
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.refresh_directories = std::move(parents);
    req.sources = std::move(paths);
    req.display_name = std::move(first_name);
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
    // A drive or a share is not copied as a whole by Ctrl+C / Ctrl+X.
    const app::SelectionCaps caps = SelectionCapsOf(s);
    if (!(move_intent ? caps.cut : caps.copy)) return;
    if (caps.kind == app::SelectionKind::Device) {
        // Phone items: the shell's own data object for them, owned by Pulse
        // (it outlives the shell host), so Pulse, Explorer and the desktop
        // can all paste them. The host's copy verb only if that fails.
        const std::vector<std::wstring> ids = SelectedShellIds(*tab);
        if (ids.empty()) return;
        if (!ops::WriteShellItemsClipboard(ids, move_intent) &&
            !RunDeviceVerb(s, ids, move_intent ? L"cut" : L"copy"))
            return;
        s.cutPaths = move_intent ? SelectedFullPaths(*tab) : std::vector<std::wstring>{};
        s.trayClipboardBatchId = 0;
        s.trayClipboardSequence = 0;
        s.trayClip = {};
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    CollectPathsToTray(s, paths, move_intent);
}

// Ctrl+C / Ctrl+X: the system clipboard only.
// The shelf offers to stage this copy until the clipboard changes.
void CollectPathsToTray(AppState& s, const std::vector<std::wstring>& paths, bool move_intent) {
    if (paths.empty()) return;
    std::vector<std::wstring> cbPaths;
    cbPaths.reserve(paths.size());
    for (const auto& p : paths) cbPaths.push_back(ClipboardPath(p));
    if (!ops::WriteClipboard(cbPaths, move_intent)) return;
    // Mirror the cut state onto the list rows (ui.md §5.2 rule 6).
    s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
    s.trayClipboardBatchId = 0;
    s.trayClipboardSequence = 0;
    s.trayClip.paths = paths;
    s.trayClip.move = move_intent;
    s.trayClip.seq = GetClipboardSequenceNumber();
    s.trayClip.at = ui::motion::NowMs();
    if (s.hwnd) {
        std::wstring title = l10n::Get(move_intent ? l10n::StringId::TrayCutOffer : l10n::StringId::TrayCopiedOffer);
        title = FillText(std::move(title), L"{n}", std::to_wstring(paths.size()));
        ShowStatusFeedback(s, std::move(title));
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

bool TrayClipLive(AppState& s) {
    if (s.trayClip.paths.empty()) return false;
    if (s.trayClip.seq != GetClipboardSequenceNumber()) {
        s.trayClip = {};
        return false;
    }
    // Nothing to offer once every copied path already sits on the tray (the
    // offer comes back if they are taken off). Big copies skip the check.
    if (s.trayClip.paths.size() > 256) return true;
    std::vector<uint64_t> staged;
    for (const auto& b : s.tray.batches())
        for (const auto& item : b.items) staged.push_back(ui::StagedPathHash(item.path));
    return std::any_of(s.trayClip.paths.begin(), s.trayClip.paths.end(), [&](const std::wstring& p) {
        return std::find(staged.begin(), staged.end(), ui::StagedPathHash(p)) == staged.end();
    });
}

size_t StagePaths(AppState& s, const std::vector<std::wstring>& paths, std::optional<bool> move_mode) {
    // A path is staged once; staging it again is a no-op.
    std::vector<std::wstring> fresh;
    fresh.reserve(paths.size());
    for (const auto& p : paths) {
        const std::wstring key = fs::NormalizePath(p);
        if (key.empty()) continue;
        bool dup = std::any_of(fresh.begin(), fresh.end(),
            [&](const std::wstring& f) { return _wcsicmp(f.c_str(), key.c_str()) == 0; });
        for (const auto& b : s.tray.batches()) {
            if (dup) break;
            dup = std::any_of(b.items.begin(), b.items.end(), [&](const app::TrayItem& it) {
                return _wcsicmp(it.path.c_str(), key.c_str()) == 0;
            });
        }
        if (!dup) fresh.push_back(key);
    }
    if (fresh.empty()) return 0;
    const bool move = move_mode.value_or(s.trayMoveMode);
    s.tray.Collect(fresh, move);
    // The batch's mode becomes the shelf's release mode (the main button).
    if (move_mode) s.trayMoveMode = *move_mode;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    return fresh.size();
}

void StageSelection(AppState& s) {
    if (DeferContentSelection(s, [](AppState& v) { StageSelection(v); })) return;
    app::Tab* tab = ActiveTab(s);
    std::vector<std::wstring> paths;
    // Same reach as Ctrl+C: drives and shares are not staged as a whole.
    // Phone items are not staged: the tray releases through Pulse's own copy.
    if (tab && tab->snapshot && !IsRecycleTab(tab) && !IsDeviceTab(tab) && SelectionCapsOf(s).copy)
        paths = SelectedFullPaths(*tab);
    if (paths.empty()) {
        if (s.hwnd)
            s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, l10n::Get(l10n::StringId::TraySelectFirst),
                                           {}, {}, {}, 2000);
        return;
    }
    StagePaths(s, paths, std::nullopt);
}

void TakeTrayClip(AppState& s) {
    if (!TrayClipLive(s)) return;
    const AppState::TrayClipOffer offer = std::move(s.trayClip);
    s.trayClip = {};
    StagePaths(s, offer.paths, offer.move);
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}
void HandleUndoHistoryAction(AppState& s, uint64_t task_id, bool undone, bool locate) {
    const auto history = s.ops.UndoHistory();
    const auto found = std::find_if(history.entries.begin(), history.entries.end(), [&](const auto& row) {
        return row.entry.task_id == task_id && row.undone == undone;
    });
    if (found == history.entries.end()) return;
    const auto& item = *found;
    if (locate) {
        const auto& paths = item.undone || item.entry.destinations.empty() ? item.entry.sources : item.entry.destinations;
        if (paths.empty()) return;
        const auto folder = fs::ParentPath(paths.front());
        if (folder.empty()) return;
        NavigateTo(s, folder);
        s.arrivalSelect = {};
        ArmArrivalSelection(s, s.pane, 0, 10000);
        s.arrivalSelect.expected_only = true;
        s.arrivalSelect.until = 0;
        for (const auto& path : paths) ExpectArrival(s, path);
        if (s.pane) if (auto* tab = s.pane->ActiveTab()) ApplyArrivalSelection(s, *s.pane, *tab, true);
        s.undoHistoryOpen = false;
    } else if (item.entry.supported) {
        if (item.undone) s.ops.RedoTo(item.entry.task_id);
        else s.ops.UndoTo(item.entry.task_id);
    }
    s.undoHistoryOffset = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void LocateOperationResults(AppState& s, uint64_t task_id) {
    const auto results = s.ops.TaskLocations(task_id);
    if (results.destinations.empty()) return;
    const std::wstring folder = fs::ParentPath(results.destinations.front());
    if (folder.empty()) return;
    RevealWindow(s);
    NavigateTo(s, folder);
    s.arrivalSelect = {};
    ArmArrivalSelection(s, s.pane, 0, 10000);
    s.arrivalSelect.expected_only = true;
    // Slow network listings may not return within the ordinary settle
    // window. The existing hard cap still bounds an absent result.
    s.arrivalSelect.until = 0;
    for (const auto& path : results.destinations) ExpectArrival(s, path);
    if (s.pane) {
        if (auto* tab = s.pane->ActiveTab())
            ApplyArrivalSelection(s, *s.pane, *tab, true);
    }
}

bool EnsureOperationWindow(AppState& s) {
    if (s.operationWindow) return true;
    const HWND hwnd = s.hwnd;
    if (!hwnd || !IsWindow(hwnd)) return false;
    ui::FileOperationCallbacks operation_callbacks;
    operation_callbacks.cancel_task = [hwnd](uint64_t task) {
        if (AppState* state = GetAppState(hwnd)) state->ops.CancelTask(task);
    };
    operation_callbacks.cancel = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) state->ops.CancelCurrent();
    };
    operation_callbacks.pause = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) state->ops.PauseCurrent();
    };
    operation_callbacks.resume = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) state->ops.ResumeCurrent();
    };
    operation_callbacks.retry_authorization = [hwnd](uint64_t task_id) {
        if (AppState* state = GetAppState(hwnd)) state->ops.ResolveAuthorization(task_id, true);
    };
    operation_callbacks.skip_authorization = [hwnd](uint64_t task_id) {
        if (AppState* state = GetAppState(hwnd)) state->ops.ResolveAuthorization(task_id, false);
    };
    operation_callbacks.retry_failed = [hwnd](uint64_t task_id, const std::vector<size_t>& items) {
        std::vector<ops::OpsManager::RetrySpawn> spawns;
        if (AppState* state = GetAppState(hwnd)) spawns = state->ops.RetryFailedItems(task_id, items);
        return spawns;
    };
    operation_callbacks.task_outcome = [hwnd](uint64_t task_id) -> std::optional<ops::OpStatus> {
        if (AppState* state = GetAppState(hwnd)) return state->ops.FinalStatus(task_id);
        return std::nullopt;
    };
    operation_callbacks.speed_history = [hwnd](uint64_t task_id) {
        if (AppState* state = GetAppState(hwnd)) return state->ops.SpeedGraph(task_id);
        return ops::OpSpeedGraph{};
    };
    operation_callbacks.open_folder = [hwnd](const std::wstring& folder) {
        if (AppState* state = GetAppState(hwnd)) NavigateTo(*state, folder);
    };
    operation_callbacks.locate_results = [hwnd](uint64_t task_id) {
        if (AppState* state = GetAppState(hwnd)) LocateOperationResults(*state, task_id);
    };
    operation_callbacks.more_action = [hwnd](uint64_t task_id, int action) {
        AppState* state = GetAppState(hwnd);
        if (!state) return;
        const auto status = state->ops.QueryTask(task_id);
        const auto locations = state->ops.TaskLocations(task_id);
        std::wstring folder;
        if (action == 2) {
            if (!locations.sources.empty()) folder = fs::ParentPath(locations.sources.front());
        } else {
            if (status) folder = status->destination_dir;
            if (folder.empty() && !locations.destinations.empty())
                folder = fs::ParentPath(locations.destinations.front());
        }
        if (folder.empty()) return;
        if (action == 3) ops::WriteClipboardText(ClipboardPath(folder));
        else if (action == 1 || action == 2) {
            RevealWindow(*state);
            NewTab(*state, folder);
        }
    };
    operation_callbacks.skip_current = [hwnd](uint64_t task, uint64_t token) {
        if (AppState* state = GetAppState(hwnd)) state->ops.SkipCurrentItem(task, token);
    };
    operation_callbacks.dismiss = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) {
            state->operationHistoryTaskId = 0;
            state->operationDismissedTaskId = state->ops.Status().task_id;
            state->operationPinnedByUser = false;
        }
    };
    // The card shrinks into the status pill and grows back out of it.
    operation_callbacks.status_anchor = [hwnd]() -> std::optional<POINT> {
        AppState* state = GetAppState(hwnd);
        if (!state || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return std::nullopt;
        const D2D1_RECT_F pill = state->renderer.TaskPillRect();
        RECT client{};
        GetClientRect(hwnd, &client);
        // No pill drawn yet: the right end of the status bar, where it appears.
        POINT point = pill.right > pill.left
            ? POINT{ static_cast<LONG>(pill.left + 60.0f * state->scale),
                     static_cast<LONG>(pill.top + 11.0f * state->scale) }
            : POINT{ client.right - static_cast<LONG>(80.0f * state->scale),
                     client.bottom - static_cast<LONG>(14.0f * state->scale) };
        ClientToScreen(hwnd, &point);
        return point;
    };
    operation_callbacks.reached_status = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) {
            state->renderer.PopTaskPill();
            if (state->framePump.Running()) state->framePump.Arm();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    };
    s.operationWindow = std::make_unique<ui::FileOperationWindow>();
    s.operationWindow->Create(hwnd, std::move(operation_callbacks));
    s.operationWindow->SetTheme(s.darkMode, s.accentColor);
    return true;
}

static bool OperationWindowVisible(const AppState& s) {
    return s.operationWindow && s.operationWindow->IsVisible();
}

static void HideOperationWindow(AppState& s) {
    if (OperationWindowVisible(s)) s.operationWindow->Hide();
}

static void ShowOperationWindow(AppState& s, const ops::OpStatus& status, bool activate) {
    const bool created = !s.operationWindow;
    if (!EnsureOperationWindow(s)) return;
    if (created) {
        s.operationWindow->Update(status);
        s.operationWindow->SetTaskStart(s.operationUiTaskId, s.operationStartedAt);
    }
    s.operationWindow->SetAutoClose(!s.operationPinnedByUser || status.active);
    s.operationWindow->Show(activate);
}

void MoveOperationUi(AppState& from, AppState& to) {
    if (&from == &to || !to.hwnd) return;
    to.operationUiTaskId = from.operationUiTaskId;
    to.operationDismissedTaskId = from.operationDismissedTaskId;
    to.conflictUiToken = from.conflictUiToken;
    to.lockPromptTaskId = from.lockPromptTaskId;
    to.operationAutoShown = from.operationAutoShown;
    to.operationPinnedByUser = from.operationPinnedByUser;
    to.operationStartedAt = from.operationStartedAt;
    to.operationFinishedAt = from.operationFinishedAt;
    const bool shown = OperationWindowVisible(from);
    if (shown) from.operationWindow->Hide();
    to.ops.SetUiWindow(to.hwnd);
    if (shown && EnsureOperationWindow(to)) {
        to.operationWindow->Update(to.ops.Status());
        to.operationWindow->SetTaskStart(to.operationUiTaskId, to.operationStartedAt);
        to.operationWindow->SetAutoClose(!to.operationPinnedByUser || to.ops.Status().active);
        to.operationWindow->Show(false);
    }
}

void PinAndShowOperationWindow(AppState& s) {
    s.operationHistoryTaskId = 0;
    if (AppState* ui = GetAppState(s.ops.UiWindow()); ui && ui != &s) MoveOperationUi(*ui, s);
    const ops::OpStatus status = s.ops.Status();

    if (!status.active && status.summary.empty() && status.last_error.empty()) return;
    if (!EnsureOperationWindow(s)) return;
    s.operationDismissedTaskId = 0;
    s.operationPinnedByUser = true;
    s.operationAutoShown = true;
    s.operationWindow->Update(status);
    s.operationWindow->SetTaskStart(s.operationUiTaskId, s.operationStartedAt);
    // Opened from the pill while it runs: it closes itself after a success
    // like any other card; opened afterwards to see the result: the user
    // closes it.
    s.operationWindow->SetAutoClose(status.active);
    s.operationWindow->Show(true, true);
}

void HandleOperationHistoryAction(AppState& s, uint64_t task_id, int action) {
    if (action == 3) {
        for (const auto& status : s.ops.RecentStatuses()) s.hiddenOperationHistory.insert(status.task_id);
        const auto current = s.ops.Status();
        if (!current.active && current.task_id) s.hiddenOperationHistory.insert(current.task_id);
        return;
    }
    if (action == 1) { LocateOperationResults(s, task_id); return; }
    if (action == 2) {
        if (s.ops.CanUndoTask(task_id)) s.ops.UndoTask(task_id);
        return;
    }
    const auto selected = s.ops.FinalStatus(task_id);
    if (!selected || !EnsureOperationWindow(s)) return;
    s.operationHistoryTaskId = task_id;
    s.operationPinnedByUser = true;
    s.operationWindow->Update(*selected);
    s.operationWindow->SetAutoClose(false);
    s.operationWindow->Show(true);
}

std::vector<ui::FluentMenuItem> BuildOperationHistoryMenu(AppState& s, const std::vector<ops::OpStatus>& history) {
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem heading;
    heading.text = l10n::Get(l10n::StringId::OpHistory); heading.heading = true;
    items.push_back(std::move(heading));
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& status = history[i];
        const int base = 100 + static_cast<int>(i) * 3;
        ui::FluentMenuItem row;
        row.text = status.summary.empty() ? status.source_label : status.summary;
        if (row.text.empty()) row.text = l10n::Get(l10n::StringId::OpDetails);
        row.glyph = status.phase == ops::OpPhase::Failed ? L"\xEA39" : L"\xE73E";
        row.shortcut = ClipboardPath(status.destination_dir);
        const auto locations = s.ops.TaskLocations(status.task_id);
        if (row.shortcut.empty() && !locations.destinations.empty())
            row.shortcut = ClipboardPath(fs::ParentPath(locations.destinations.front()));
        row.tooltip = row.shortcut;
        ui::FluentMenuItem details;
        details.command = base; details.text = l10n::Get(l10n::StringId::OpDetails);
        row.children.push_back(std::move(details));
        ui::FluentMenuItem locate;
        locate.command = base + 1; locate.text = l10n::Get(l10n::StringId::OpLocateResults);
        locate.enabled = !locations.destinations.empty();
        row.children.push_back(std::move(locate));
        ui::FluentMenuItem undo;
        undo.command = base + 2; undo.text = l10n::Get(l10n::StringId::Undo);
        undo.enabled = s.ops.CanUndoTask(status.task_id);
        row.children.push_back(std::move(undo));
        items.push_back(std::move(row));
    }
    if (history.empty()) {
        ui::FluentMenuItem empty;
        empty.text = l10n::Get(l10n::StringId::NoRecent); empty.enabled = false;
        items.push_back(std::move(empty));
    }
    ui::FluentMenuItem clear;
    clear.command = 1; clear.text = l10n::Get(l10n::StringId::OpClearCompleted);
    clear.enabled = !history.empty(); items.push_back(std::move(clear));
    return items;
}

void HandleOperationStatusAction(AppState& s, int action) {
    const auto current = s.ops.Status();
    if (action == 0) {
        if (s.ops.CanUndoTask(current.task_id)) s.ops.UndoTask(current.task_id);
        return;
    }
    if (action == 1) { LocateOperationResults(s, current.task_id); return; }
    if (action == 2) {
        if (current.active && current.can_pause) {
            if (current.pause_requested || current.phase == ops::OpPhase::Paused) s.ops.ResumeCurrent();
            else s.ops.PauseCurrent();
        }
        return;
    }
    if (current.active || current.phase == ops::OpPhase::Failed) { PinAndShowOperationWindow(s); return; }
    if (!s.menu) return;
    auto history = s.ops.RecentStatuses();
    std::erase_if(s.hiddenOperationHistory, [&](uint64_t task_id) {
        return task_id != current.task_id && std::none_of(history.begin(), history.end(),
            [&](const auto& status) { return status.task_id == task_id; });
    });
    if (current.task_id && std::none_of(history.begin(), history.end(),
        [&](const auto& status) { return status.task_id == current.task_id; })) history.insert(history.begin(), current);
    std::erase_if(history, [&](const auto& status) { return s.hiddenOperationHistory.contains(status.task_id); });
    auto items = BuildOperationHistoryMenu(s, history);
    POINT point{}; GetCursorPos(&point);
    const int command = s.menu->TrackPopup(point, std::move(items));
    if (command == 1) {
        HandleOperationHistoryAction(s, 0, 3);
        return;
    }
    if (command < 100) return;
    const auto index = static_cast<size_t>((command - 100) / 3);
    if (index >= history.size()) return;
    const auto& selected = history[index];
    HandleOperationHistoryAction(s, selected.task_id, (command - 100) % 3);
}

// Both batch-rename entries (the selection and the tray) submit through here.
// Nothing else changes up front: places and the tray follow the renames that
// actually succeeded once the operation completes (app_main's completion
// handler), so a failed or cancelled item never leaves a stale new name behind.
// A rename task started from the batch rename dialog (scene 5): the dialog
// shows it going through, then the list flashes the new names and a snack
// offers undo. One per window; the dialog is modal.
struct BatchRenameNote {
    uint64_t task = 0;
    std::wstring folder;
    std::vector<std::wstring> names;   // new names
    bool dialog_open = false;
    bool finished = false;
    bool success = false;
};
static std::unordered_map<const AppState*, BatchRenameNote> g_batch_rename;

static void ShowBatchRenameDone(AppState& s) {
    auto it = g_batch_rename.find(&s);
    if (it == g_batch_rename.end()) return;
    const BatchRenameNote note = std::move(it->second);
    g_batch_rename.erase(it);
    // A failure is reported by the operations UI; the rows that did change
    // still come back with the list refresh.
    if (!note.success || !s.hwnd) return;
    // The renamed rows flash in place; the status bar's undo chip offers the
    // step back, so no capsule repeats it.
    AiListFlashRows(note.folder, note.names);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NoteBatchRenameFinished(AppState& s, const ops::FinishedTask& done) {
    auto it = g_batch_rename.find(&s);
    if (it == g_batch_rename.end() || done.task_id != it->second.task) return;
    it->second.finished = true;
    it->second.success = done.success;
    if (!it->second.dialog_open) ShowBatchRenameDone(s);   // else when it closes
}

// 一句话生成步骤: one streamed request to the assistant's model service.
class BatchRenameAiRequest : public ui::BatchRenameAiTask {
public:
    explicit BatchRenameAiRequest(std::shared_ptr<app::ai::ModelStream> stream) : stream_(std::move(stream)) {}
    ~BatchRenameAiRequest() override { if (stream_) stream_->Cancel(); }

    bool Poll(std::string& reply, std::wstring& error) override {
        std::vector<app::ai::ModelEvent> events;
        stream_->Drain(events);
        for (auto& e : events) {
            switch (e.kind) {
            case app::ai::ModelEvent::Kind::Content: text_ += e.text; break;
            case app::ai::ModelEvent::Kind::Finished:
                reply = e.content.empty() ? text_ : e.content;
                return true;
            case app::ai::ModelEvent::Kind::Failed:
                error = Reason(e);
                return true;
            default: break;
            }
        }
        if (stream_->Ended()) { error = l10n::Get(l10n::StringId::BrAiErrNetwork); return true; }
        return false;
    }
    void Cancel() override { stream_->Cancel(); }

private:
    static std::wstring Reason(const app::ai::ModelEvent& e) {
        switch (e.http_status) {
        case 0: return l10n::Get(l10n::StringId::BrAiErrNetwork);
        case 401: case 403: return l10n::Get(l10n::StringId::BrAiErrKey);
        case 402: return l10n::Get(l10n::StringId::BrAiErrBalance);
        case 429: case 500: case 502: case 503: return l10n::Get(l10n::StringId::BrAiErrBusy);
        default: {
            wchar_t buf[128]{};
            _snwprintf_s(buf, _TRUNCATE, l10n::Get(l10n::StringId::BrAiErrHttp).c_str(), e.http_status);
            return buf;
        }
        }
    }
    std::shared_ptr<app::ai::ModelStream> stream_;
    std::string text_;
};

static size_t SubmitBatchRename(AppState& s, const std::vector<std::wstring>& paths, app::Tab* select_in,
                                const app::rename::Preset* preset = nullptr) {
    if (paths.size() < 2) return 0;
    ui::BatchRenameDialogOptions options;
    if (preset) options.preset = *preset;
    size_t renamed = 0;
    // The assistant's connection and model (ids shown and sent as listed).
    if (AiAgentConnected()) {
        options.ai_model = AiChatModelId(s);
        options.ai_start = [](const std::string& body, std::wstring& error) -> std::shared_ptr<ui::BatchRenameAiTask> {
            std::wstring key;
            if (!app::ai::LoadModelKey(key)) { error = l10n::Get(l10n::StringId::BrAiNoConn); return nullptr; }
            app::ai::ModelEndpoint endpoint;
            endpoint.base_url = AiAgentBaseUrl();
            endpoint.key = std::move(key);
            auto stream = app::ai::ModelStream::Start(std::move(endpoint),
                                                      app::ai::WithoutThinking(app::ai::CurrentProvider(), body));
            if (!stream) { error = l10n::Get(l10n::StringId::BrAiErrNetwork); return nullptr; }
            return std::make_shared<BatchRenameAiRequest>(std::move(stream));
        };
        options.ai_provider = app::ai::ProviderName(app::ai::CurrentProvider());
    }
    // 按内容命名: text through the content-search reader (PDF and Office in
    // the sandboxed parser), never the file itself.
    options.can_read = [](const std::wstring& ext) { return index::IsIndexedContentExtension(ext); };
    options.read_texts = [](const std::vector<std::wstring>& paths, size_t max_chars,
                            const std::function<bool()>& cancelled, const std::function<void(size_t, std::wstring)>& done) {
        index::DocumentReadSession session;   // one warm parser for the whole pass
        for (size_t i = 0; i < paths.size(); ++i) {
            if (cancelled()) return;
            const std::wstring& path = paths[i];
            const size_t dot = path.find_last_of(L".\\/");
            const std::wstring ext = dot != std::wstring::npos && path[dot] == L'.' ? path.substr(dot) : std::wstring();
            // Documents: the reader's own size limit; plain text: 16 MB.
            const uint64_t cap = index::IsExtractedDocumentExtension(ext) ? UINT64_MAX : (16ull << 20);
            std::wstring text;
            uint64_t bytes = 0;
            DWORD error = 0;
            if (!index::ReadSearchableDocument(path, cap, text, bytes, &error, text::Encoding::Auto, cancelled)) text.clear();
            if (text.size() > max_chars * 2) {
                text.resize(max_chars * 2);
                if (text.back() >= 0xD800 && text.back() <= 0xDBFF) text.pop_back();
            }
            done(i, std::move(text));
        }
    };
    options.execute = [&s, select_in, &renamed](const std::vector<app::BatchRenameItem>& items) -> uint64_t {
        ops::OpRequest req;
        req.type = ops::OpType::BatchRename;
        for (const auto& item : items) {
            if (item.status != app::BatchRenameStatus::Ok) continue;
            req.sources.push_back(item.source_path);
            req.new_names.push_back(item.new_name);
        }
        if (req.sources.empty()) return 0;
        if (select_in) {
            select_in->pending_selected_names = req.new_names;
            select_in->pending_ensure_selection_visible = true;
            select_in->pending_selected_name = req.new_names.front();
        }
        BatchRenameNote note;
        note.folder = fs::ParentPath(req.sources.front());
        note.names = req.new_names;
        note.dialog_open = true;
        note.task = s.ops.Submit(std::move(req));
        if (!note.task) return 0;
        const uint64_t task = note.task;
        renamed = note.names.size();
        g_batch_rename[&s] = std::move(note);
        return task;
    };
    options.poll = [&s](uint64_t task) {
        ui::BatchRenameProgress p;
        if (auto it = g_batch_rename.find(&s); it != g_batch_rename.end() && it->second.task == task)
            p.finished = it->second.finished;
        if (!p.finished) {
            const ops::OpStatus st = s.ops.Status();
            if (st.task_id == task) p.done = static_cast<size_t>(st.completed_items);
        }
        return p;
    };
    const auto result = ui::ShowBatchRenameDialog(s.hwnd, paths, s.darkMode, s.accentColor, options);
    if (result.executed) {
        if (auto it = g_batch_rename.find(&s); it != g_batch_rename.end()) {
            it->second.dialog_open = false;
            if (it->second.finished) ShowBatchRenameDone(s);
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return renamed;
    }
    if (!result.accepted) return 0;
    ops::OpRequest req;
    req.type = ops::OpType::BatchRename;
    for (const auto& item : result.items) {
        if (item.status != app::BatchRenameStatus::Ok) continue;
        req.sources.push_back(item.source_path);
        req.new_names.push_back(item.new_name);
    }
    if (req.sources.empty()) return 0;
    if (select_in) {
        select_in->pending_selected_names = req.new_names;
        select_in->pending_ensure_selection_visible = true;
        select_in->pending_selected_name = req.new_names.front();
    }
    renamed = req.sources.size();
    s.ops.Submit(std::move(req));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return renamed;
}

void ShowBatchRename(AppState& s) {
    if(DeferContentSelection(s,[=](AppState& v){ShowBatchRename(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly ||
        app::ComputeSelectionCaps(*tab, &s.places).kind != app::SelectionKind::Files)
        return;
    SubmitBatchRename(s, SelectedFullPaths(*tab), tab);
}
void ShowBatchRenamePaths(AppState& s, const std::vector<std::wstring>& paths) {
    SubmitBatchRename(s, paths, nullptr);
}
size_t ShowBatchRenameWith(AppState& s, const std::vector<std::wstring>& paths, const app::rename::Preset& preset,
                           app::Tab* select_in) {
    return SubmitBatchRename(s, paths, select_in, &preset);
}

void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog) {
    // A notification queued for the previous operations window (M4c) goes on
    // to the window that shows operations now.
    if (AppState* ui = GetAppState(s.ops.UiWindow()); ui && ui != &s) {
        UpdateOperationWindow(*ui, allow_conflict_dialog);
        return;
    }

    const auto history = s.ops.UndoHistory();
    if (history.completed_sequence != s.undoHistoryNotified) {
        s.undoHistoryNotified = history.completed_sequence;
        if (history.completed_steps > 0 || !history.completed_success) {
            wchar_t title[128]{};
            swprintf_s(title, l10n::Get(history.completed_redo ? l10n::StringId::HistoryRedoneSteps :
                l10n::StringId::HistoryUndoneSteps).c_str(), static_cast<unsigned>(history.completed_steps));
            std::vector<ui::SnackAction> actions;
            if (!history.completed_redo && s.ops.CanRedo())
                actions.push_back({l10n::Get(l10n::StringId::HistoryRedo), WM_HISTORY_REDO});
            if (history.completed_redo && s.ops.CanUndo())
                actions.push_back({l10n::Get(l10n::StringId::SnackUndo), WM_UNDO_DELETE});
            s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, title,
                history.completed_success ? L"" : l10n::Get(l10n::StringId::HistoryStopped), std::move(actions));
        }
    }
    // Built on first show (EnsureOperationWindow): most windows never need one.
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    if (!s.appPrefs.status_hints_learned && (status.task_id != 0 || !history.entries.empty())) {
        s.appPrefs.status_hints_learned = true;
        s.appPrefs.Save();
    }

    if (s.operationHistoryTaskId && !status.active) return;
    if (status.active) s.operationHistoryTaskId = 0;
    if (status.active && status.task_id != s.operationUiTaskId) {
        s.operationUiTaskId = status.task_id;
        s.operationAutoShown = false;
        s.operationPinnedByUser = false;
        s.operationStartedAt = now;
        s.operationFinishedAt = {};
    }

    // Creation stays in the list (including inline rename after completion).
    // UAC already owns consent; do not turn it into a transfer progress window.
    const bool inline_create = status.type == ops::OpType::CreateFolder ||
                               status.type == ops::OpType::CreateTextFile;
    if (inline_create && !s.operationPinnedByUser && status.phase != ops::OpPhase::Failed) {
        HideOperationWindow(s);
        return;
    }
    if (s.operationWindow) s.operationWindow->Update(status);

    if (allow_conflict_dialog) {
        if (const auto conflict = s.ops.PendingConflict();
            conflict && conflict->token != s.conflictUiToken) {
            s.conflictUiToken = conflict->token;
            const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                s.hwnd, *conflict, s.darkMode, s.accentColor);
            s.ops.ResolveConflict(conflict->token, result.choice, result.apply_to_all);
        }
    }

    // A failure inside the assistant's task: the panel asks instead.
    if (!status.active && status.phase == ops::OpPhase::Failed && status.task_id != 0 &&
        status.task_id != s.lockPromptTaskId && AiAssistantOwnsTask(status.task_id)) {
        s.lockPromptTaskId = status.task_id;
        s.operationDismissedTaskId = status.task_id;
        s.operationPinnedByUser = false;
        HideOperationWindow(s);
        return;
    }

    // Failed on an item other processes hold: name them and offer the retry
    // instead of the plain failure window (the token guards modal re-entry).
    const bool locked_failure = !status.active && status.phase == ops::OpPhase::Failed
        && !status.lock_owners.empty() && status.task_id != 0
        && !(s.operationWindow && s.operationWindow->TracksRetry(status.task_id));
    if (locked_failure && allow_conflict_dialog && status.task_id != s.lockPromptTaskId) {
        s.lockPromptTaskId = status.task_id;
        s.operationDismissedTaskId = status.task_id;
        s.operationPinnedByUser = false;
        HideOperationWindow(s);
        PromptLockedItem(s, status);
        return;
    }

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId &&
        !s.operationPinnedByUser) {
        HideOperationWindow(s);
        return;
    }

    if (status.active && status.authorization != ops::AuthorizationState::None) {
        // Keep the existing operation window visible without stealing focus from UAC.
        if (!OperationWindowVisible(s)) ShowOperationWindow(s, status, false);
        return;
    }

    if (status.active) {
        // Progress starts in the status-bar pill; a task still running after
        // kOperationAutoShowDelay opens the details window once (without
        // taking focus), unless it is nearly done. Closing that window keeps
        // it closed for this task (operationDismissedTaskId above).
        // Emptying the Recycle Bin starts from a menu with no row to watch, so
        // its window opens sooner.
        constexpr auto kOperationAutoShowDelay = std::chrono::milliseconds(2000);
        const bool quick_type = status.type == ops::OpType::Rename ||
                                status.type == ops::OpType::CreateFolder ||
                                status.type == ops::OpType::CreateTextFile;
        const bool emptying = status.type == ops::OpType::EmptyRecycle;
        const auto delay = emptying ? std::chrono::milliseconds(400) : kOperationAutoShowDelay;
        const bool nearly_done = !emptying && status.percent >= 90.0f;
        if (!quick_type && !s.operationAutoShown && !nearly_done &&
            status.phase != ops::OpPhase::WaitingForConflict &&
            now - s.operationStartedAt >= delay) {
            s.operationAutoShown = true;
            if (!OperationWindowVisible(s)) ShowOperationWindow(s, status, false);
        }
        return;
    }

    if (s.operationPinnedByUser) return;

    if (status.phase == ops::OpPhase::Completed) {
        // The live window closes immediately on success; retain completion time only.
        if (s.operationFinishedAt.time_since_epoch().count() == 0)
            s.operationFinishedAt = now;
    } else if (status.phase == ops::OpPhase::Failed) {
        // The ops notification shows the locked-item prompt; a timer tick
        // that sees the failure first must not open the failure window.
        if (locked_failure) return;
        if (ErrorIs(status.last_error, L"已取消", L"Canceled")) {
            HideOperationWindow(s);
            return;
        }
        const bool simple = status.type == ops::OpType::CreateFolder
                         || status.type == ops::OpType::CreateTextFile
                         || status.type == ops::OpType::Rename
                         || status.type == ops::OpType::BatchRename;
        if (simple) {
            HideOperationWindow(s);
            if (status.task_id == 0 || status.task_id == s.operationDismissedTaskId)
                return;
            s.operationDismissedTaskId = status.task_id;
            const bool folder = status.type == ops::OpType::CreateFolder;
            const bool file = status.type == ops::OpType::CreateTextFile;
            const std::wstring title = l10n::Get(folder ? l10n::StringId::CannotCreateFolder
                : file ? l10n::StringId::CannotCreateFile : l10n::StringId::CannotRename);
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
        if (!OperationWindowVisible(s)) ShowOperationWindow(s, status, true);
    }
}

} // namespace pulse
