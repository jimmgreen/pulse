#include "app_internal.h"
#include "../ops/clipboard.h"
#include "../ui/quick_preview_command.h"
#include "../fs/cloud_state.h"
#include "cloud_download.h"
#include "cloud_roots.h"
#include "../common/localization.h"
#include "app_hover_preview.h"
#include "app_input.h"

namespace pulse {
namespace {
// One mouse, so one hold at a time across Pulse's windows.
struct HoldPeek {
    HWND hwnd = nullptr;
    bool armed = false;     // the timer runs
    bool shown = false;     // the preview is up because of the hold
    int index = -1;
    std::wstring path;
    POINT down{};
};
HoldPeek g_hold;
} // namespace

void ArmHoldPreview(AppState& s, int index, POINT client) {
    const int delay = app::NormalizeHoldPreview(s.appPrefs.hold_preview_ms);
    if (delay <= 0 || !s.hwnd || s.quickPreview.visible() || index < 0) return;
    if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT) | GetKeyState(VK_MENU)) & 0x8000) return;
    // Touch and pen presses are left alone (press-and-hold is their right click).
    if ((GetMessageExtraInfo() & 0xFFFFFF00) == 0xFF515700) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || index >= static_cast<int>(tab->EntryCount())) return;
    const auto& entry = tab->EntryAt(static_cast<size_t>(index));
    if (entry.is_dir) return;
    g_hold = HoldPeek{};
    g_hold.hwnd = s.hwnd;
    g_hold.armed = true;
    g_hold.index = index;
    g_hold.path = EntryFullPath(*tab, index);
    g_hold.down = client;
    if (g_hold.path.empty()) { g_hold = HoldPeek{}; return; }
    SetTimer(s.hwnd, kHoldPreviewTimer, static_cast<UINT>(delay), nullptr);
}

void HoldPreviewMouseMove(AppState& s, POINT client) {
    if (!g_hold.armed || g_hold.hwnd != s.hwnd) return;
    if (std::abs(client.x - g_hold.down.x) > GetSystemMetrics(SM_CXDRAG) ||
        std::abs(client.y - g_hold.down.y) > GetSystemMetrics(SM_CYDRAG)) {
        KillTimer(s.hwnd, kHoldPreviewTimer);
        g_hold = HoldPeek{};
    }
}

void HoldPreviewButtonUp(AppState& s) {
    if (g_hold.hwnd != s.hwnd) return;
    if (g_hold.armed) KillTimer(s.hwnd, kHoldPreviewTimer);
    const bool shown = g_hold.shown;
    g_hold = HoldPeek{};
    if (shown && s.quickPreview.visible()) s.quickPreview.Close();
}

void HoldPreviewTimer(AppState& s) {
    KillTimer(s.hwnd, kHoldPreviewTimer);
    if (!g_hold.armed || g_hold.hwnd != s.hwnd) return;
    g_hold.armed = false;
    // Still held? The queue's view of the left button (in step with the
    // WM_LBUTTONUP that would have cancelled this), and Pulse still has the
    // capture the press took: a release it never saw ends that.
    const bool held = (GetKeyState(VK_LBUTTON) & 0x8000) != 0 && GetCapture() == s.hwnd;
    if (!held || s.quickPreview.visible()) {
        g_hold = HoldPeek{};
        return;
    }
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || g_hold.index >= static_cast<int>(tab->EntryCount()) ||
        EntryFullPath(*tab, g_hold.index) != g_hold.path) {
        g_hold = HoldPeek{};
        return;
    }
    tab->SelectOnly(g_hold.index);
    ui::QuickPreviewItem item;
    if (!SelectedQuickPreviewItem(s, item)) {
        g_hold = HoldPeek{};
        return;
    }
    // The press became a peek: its release neither opens, renames, drags
    // nor draws a marquee.
    CancelFileClick(s);
    CancelListSelection(s);
    HideHoverPreview(s);
    g_hold.shown = true;
    OpenQuickPreview(s, item, false);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HoldPreviewPin(AppState& s) {
    if (!g_hold.shown || g_hold.hwnd != s.hwnd || !s.quickPreview.visible()) return false;
    g_hold = HoldPeek{};
    s.quickPreview.Activate();
    return true;
}

void StartCloudPreviewDownload(AppState& s, const std::wstring& path, uint64_t size) {
    if (path.empty() || !s.hwnd) return;
    // Only ever for a file that is really online-only right now: the button
    // can be a frame stale, and a folder would make the provider fetch a listing.
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        !fs::IsCloudPlaceholder(attrs)) return;
    app::StartCloudDownload(path, size, s.hwnd, WM_CLOUD_DOWNLOAD);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HandleCloudDownloadMessage(AppState& s, WPARAM event, LPARAM job) {
    if (!s.hwnd) return;
    // Repaint: BuildVm hands the percent to the details well and Quick Look.
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (event != app::kCloudDownloadFailed) {
        // Done: the provider clears the placeholder bits; the folder watcher
        // brings the new attributes, which re-key the preview cache, and
        // SyncQuickPreview reloads Quick Look on the attribute change.
        return;
    }
    std::wstring provider = app::CloudRootLabelFor(app::CloudDownloadPathFor(job));
    if (provider.empty()) provider = l10n::Get(l10n::StringId::CloudProviderGeneric);
    std::wstring title = l10n::Get(l10n::StringId::CloudDownloadFailed);
    if (const size_t at = title.find(L"{provider}"); at != std::wstring::npos) title.replace(at, 10, provider);
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, std::move(title), {}, {}, {}, 3000);
}

ui::QuickPreviewOptions QuickPreviewOptionsFromPrefs(const app::AppPrefs& prefs) {
    ui::QuickPreviewOptions options;
    options.size_mode = app::NormalizePreviewSize(prefs.preview_size);
    options.remember_size = prefs.preview_remember_size;
    if (prefs.preview_rect_w > 0 && prefs.preview_rect_h > 0)
        options.remembered = RECT{prefs.preview_rect_x, prefs.preview_rect_y,
                                  prefs.preview_rect_x + prefs.preview_rect_w,
                                  prefs.preview_rect_y + prefs.preview_rect_h};
    options.space_closes = prefs.preview_space_closes;
    options.loop = app::NormalizePreviewLoop(prefs.preview_loop);
    options.key_hint_left = std::clamp(prefs.preview_key_hint_left, 0, 3);
    return options;
}

// What the window changed itself (size menu, L, a dragged size) goes to the
// prefs; Settings shows it from there.
static void SaveQuickPreviewOptions(AppState& s, ui::QuickPreviewWindow& preview) {
    const ui::QuickPreviewOptions& options = preview.options();
    if (options == QuickPreviewOptionsFromPrefs(s.appPrefs)) return;
    auto& prefs = s.appPrefs;
    prefs.preview_size = app::NormalizePreviewSize(options.size_mode);
    prefs.preview_remember_size = options.remember_size;
    prefs.preview_rect_x = options.remembered.left;
    prefs.preview_rect_y = options.remembered.top;
    prefs.preview_rect_w = (std::max)(0L, options.remembered.right - options.remembered.left);
    prefs.preview_rect_h = (std::max)(0L, options.remembered.bottom - options.remembered.top);
    prefs.preview_space_closes = options.space_closes;
    prefs.preview_loop = app::NormalizePreviewLoop(options.loop);
    prefs.preview_key_hint_left = std::clamp(options.key_hint_left, 0, 3);
    prefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);   // Settings may be showing these
}

void HandleQuickPreviewCommand(AppState& s, const ui::QuickPreviewCommand& command) {
    HandleQuickPreviewCommand(s, s.quickPreview, command);
}

void HandleQuickPreviewCommand(AppState& s, ui::QuickPreviewWindow& preview,
                               const ui::QuickPreviewCommand& command) {
    if (command.action == ui::QuickPreviewAction::SaveOptions) {
        SaveQuickPreviewOptions(s, preview);
        return;
    }
    if (!preview.visible() || command.target.path.empty()) return;
    if (command.action == ui::QuickPreviewAction::InstallMediaPack) {
        // Same download as Settings > 预览增强包; BuildVm feeds its progress
        // back to the card and reopens the preview once it is installed.
        s.settings.InstallMediaPack(s.hwnd);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (command.action == ui::QuickPreviewAction::InstallImagePack) {
        s.settings.InstallImagePack(s.hwnd);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (command.action == ui::QuickPreviewAction::DownloadCloud) {
        StartCloudPreviewDownload(s, command.target.path, command.target.size);
        return;
    }
    const auto& target = command.target;
    const auto action = command.action;
    if (action == ui::QuickPreviewAction::OpenInNewTab) {
        if (!(target.attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
        preview.Close();
        OpenFolderTab(s, target.path);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (target.read_only && (action == ui::QuickPreviewAction::Cut ||
        action == ui::QuickPreviewAction::Rename || action == ui::QuickPreviewAction::Delete)) return;

    if (command.folder_child) {
        switch (action) {
        case ui::QuickPreviewAction::Open: OpenPath(s, target.path); break;
        case ui::QuickPreviewAction::Copy:
        case ui::QuickPreviewAction::Cut:
            CollectPathsToTray(s, {target.path}, action == ui::QuickPreviewAction::Cut);
            break;
        case ui::QuickPreviewAction::CopyPath:
            ops::WriteClipboardText(ClipboardPath(target.path));
            break;
        case ui::QuickPreviewAction::ToggleStar:
            ToggleStarred(s, target.path, target.attrs & FILE_ATTRIBUTE_DIRECTORY
                ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
            break;
        case ui::QuickPreviewAction::Properties: s.ops.ShowProperties(target.path); break;
        case ui::QuickPreviewAction::Delete:
            DeletePaths(s, {target.path}, command.shift);
            break;
        case ui::QuickPreviewAction::Rename: {
            // Reuse the existing inline editor in the item's containing folder.
            preview.Close();
            const auto parent = fs::ParentPath(target.path);
            NavigateTo(s, parent);
            if (auto* tab = ActiveTab(s); tab && tab->current_path == fs::NormalizePath(parent)) {
                if (!tab->loading && tab->snapshot) {
                    for (size_t i = 0; i < tab->EntryCount(); ++i) {
                        if (tab->EntryAt(i).name != target.name) continue;
                        tab->SelectOnly(static_cast<int>(i));
                        ShowRenameOverlay(s);
                        return;
                    }
                }
                tab->pending_preview_rename = target.name;
            }
            break;
        }
        default: break;
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }

    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    int index = -1;
    for (size_t i = 0; i < tab->EntryCount(); ++i) {
        const auto& entry = tab->EntryAt(i);
        if (entry.name == target.name && EntryFullPath(*tab, static_cast<int>(i)) == target.path) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) return;
    tab->SelectOnly(index);
    EnsureRowVisible(s, *tab, index);
    switch (action) {
    case ui::QuickPreviewAction::Open: OpenSelected(s); break;
    case ui::QuickPreviewAction::Cut: CollectToTray(s, true); break;
    case ui::QuickPreviewAction::Copy: CollectToTray(s, false); break;
    case ui::QuickPreviewAction::CopyPath: CopySelectedPath(s); break;
    case ui::QuickPreviewAction::ToggleStar:
        ToggleStarred(s, target.path, target.attrs & FILE_ATTRIBUTE_DIRECTORY
            ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
        preview.SetStarred(s.places.IsStarred(target.path));
        break;
    case ui::QuickPreviewAction::Rename:
        preview.Close();
        ShowRenameOverlay(s);
        break;
    case ui::QuickPreviewAction::Delete:
        if (&preview == &s.quickPreview)
            s.quickPreviewAnchorView = BuildVm(s).pane.ViewIndex(index);
        DeleteSelected(s, command.shift);
        break;
    case ui::QuickPreviewAction::Properties: s.ops.ShowProperties(target.path); break;
    default: break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
} // namespace pulse
