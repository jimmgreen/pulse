#include "tag_color_commands.h"
#include "git_ui.h"
#include "mtime_hint_prefs.h"
#include "tag_ads_sync.h"
#include "tag_query.h"
#include "folder_alias.h"
#include "zip_compress_ui.h"
#include "keymap.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"
#include "../ui/known_extensions.h"
#include <atomic>
#include "../ui/shortcut_help.h"
// app_commands.cpp — extracted from app_main.cpp.
#include "quick_access.h"
#include "app_column_view.h"
#include "../ui/toolbar_layout.h"
#include "../ui/command_icons.h"
#include "app_internal.h"
#include "app_sidebar_refresh.h"
#include "favorites_bar.h"
#include "tray_reveal.h"
#include "group_wheel_ui.h"
#include "text_diff.h"
#include "global_search_controller.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/folder_picker_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/toolbar_picker.h"
#include "../ui/toolbar_pin_drag.h"
#include "../ui/advanced_search_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "tag_missing.h"
#include "tag_rules.h"
#include "entry_sort.h"
#include "session.h"
#include "context_menu.h"
#include "actions/action_registry.h"
#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "system_ctx_registry.h"
#include "shell_verbs.h"
#include "toolbar_apps.h"
#include "toolbar_tools.h"
#include "toolbar_selection.h"
#include "vertical_tabs.h"
#include "app_change_tracking.h"
#include "batch_rename.h"
#include "search_query.h"
#include <limits>
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "../fs/device_path.h"
#include "device_registry.h"
#include "shell_registry_watch.h"
#include "search_refresh_log.h"
#include "selection_caps.h"
#include "drive_verbs.h"
#include "cloud_roots.h"
#include "../fs/cloud_pin.h"
#include "../fs/cloud_state.h"
#include "bitlocker_unlock.h"
#include "../fs/bitlocker_volume.h"
#include "app_navigation.h"
#include "app_network_locations.h"
#include "app_network_page.h"
#include "app_messages.h"
#include "../ui/confirm_dialog.h"
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
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "pulse_version.h"

using namespace pulse;

namespace pulse {
app::ShellRegistryWatcher g_shell_watcher;
std::atomic<uint32_t> g_shell_seed_generation{0};

namespace {
// Menu-cache key prefix for phone items: their rows are the shell's, not the
// registry verbs of a local file with the same extension.
constexpr wchar_t kDeviceVerbKeyPrefix[] = L"device:";
// A phone's own row in This PC (one selected): the device's parsing name, so
// its menu is the shell's (导入图片和视频, 固定到"开始", 创建快捷方式) and 属性
// opens the device's sheet. Empty for anything else, the "no phone" tile and
// test phones (no shell item behind them).
std::wstring PcDeviceShellId(const app::Tab& tab, const std::vector<int>& indices) {
    if (!tab.current_path.empty() || !tab.snapshot || indices.size() != 1) return {};
    const int index = indices.front();
    if (index < 0 || index >= static_cast<int>(tab.EntryCount())) return {};
    const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(index));
    if (!e.pc_device || (e.pc_device & fs::kPcDevicePlaceholder)) return {};
    const auto where = fs::ParseDevicePath(e.full_path);
    if (!where) return {};
    for (const auto& device : app::DeviceRegistry::Instance().Devices())
        if (device.key == where->device && device.id.starts_with(L"::")) return device.id;
    return {};
}
std::wstring TagLabel(l10n::StringId id, const std::wstring& name, size_t count = 0) {
    const auto& pattern = l10n::Get(id);
    const int length = _scwprintf(pattern.c_str(), name.c_str(), count);
    if (length < 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    swprintf_s(text.data(), text.size(), pattern.c_str(), name.c_str(), count);
    text.resize(static_cast<size_t>(length));
    return text;
}
} // namespace

bool EnsureMenu(AppState& s) {
    if (!s.menu) {
        s.menu = std::make_unique<ui::FluentMenu>();
        if (!s.menu->Create(s.hwnd, &s.compositor, s.scale)) {
            s.menu.reset();
            return false;
        }
    }
    s.menu->SetTheme(s.darkMode, s.accentColor);
    s.menu->SetRowHeightDip(static_cast<float>(app::MenuRowHeightDip(
        app::EffectiveRowHeightDip(s.appPrefs.row_height, s.appPrefs.ui_font_scale))));
    return true;
}

void CopySelectedPath(AppState& s) {
    if(DeferContentSelection(s,[=](AppState& v){CopySelectedPath(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i) text += L"\r\n";
        text += fs::IsDevicePath(paths[i]) ? DeviceFriendlyPath(paths[i]) : ClipboardPath(paths[i]);
    }
    ops::WriteClipboardText(text);
}

// Creates "新建文件夹"/"新建文本文档.txt" via the ops layer, then (on the next
// snapshot) selects it and enters the rename overlay.
// 新建文件夹 in a writable phone folder: named apart from the rows listed,
// made by Explorer's engine, then renamed in place like a local one (the
// completion names its pulse:device: path). A phone takes no empty files.
static void CreateDeviceFolder(AppState& s, app::Tab& tab) {
    auto& devices = app::DeviceRegistry::Instance();
    if (!devices.FolderWritable(tab.current_path)) return;
    const std::wstring parent_id = devices.FolderShellId(tab.current_path);
    if (parent_id.empty()) return;
    const std::wstring base = l10n::Get(l10n::StringId::NewFolder).c_str();
    const auto taken = [&](const std::wstring& name) {
        for (size_t i = 0; i < tab.EntryCount(); ++i)
            if (_wcsicmp(tab.EntryAt(i).name.c_str(), name.c_str()) == 0) return true;
        return false;
    };
    std::wstring name = base;
    for (unsigned index = 2; taken(name) && index < 10000; ++index)
        name = base + L" (" + std::to_wstring(index) + L")";
    ops::OpRequest req;
    req.type = ops::OpType::CreateFolder;
    req.dest_dir = parent_id;
    req.new_name = name;
    req.refresh_directories.push_back(tab.current_path);
    const auto task_id = s.ops.Submit(std::move(req));
    app::QueueCreateRenameIntent(tab.create_rename_intents, task_id, tab.current_path, tab.view_generation);
}

void CreateNewItem(AppState& s, bool folder) {
    app::Tab* tab = ActiveTab(s);
    if (folder && IsDeviceTab(tab)) {
        CreateDeviceFolder(s, *tab);
        return;
    }
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly ||
        app::IsUncServerRoot(tab->current_path))
        return;
    // 自定义工具栏 "remember the last type": the New button follows what was made last.
    if (s.appPrefs.toolbar_last_new != (folder ? 0 : 1)) {
        s.appPrefs.toolbar_last_new = folder ? 0 : 1;
        if (!s.isolatedTest) s.appPrefs.Save();
    }
    std::wstring name = folder
        ? app::UniqueChildName(tab->current_path,
            l10n::Get(l10n::StringId::NewFolder), L"")
        : app::UniqueChildName(tab->current_path,
            l10n::Get(l10n::StringId::NewTextDocument), L".txt");
    std::wstring full = tab->current_path;
    if (!full.ends_with(L"\\")) full += L"\\";
    full += name;
    ops::OpRequest req;
    req.type = folder ? ops::OpType::CreateFolder : ops::OpType::CreateTextFile;
    req.sources.push_back(full);
    const auto task_id = s.ops.Submit(std::move(req));
    app::QueueCreateRenameIntent(tab->create_rename_intents, task_id,
        tab->current_path, tab->view_generation);
}

namespace {
bool CanCreateHere(const app::Tab* tab) {
    return tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) && !tab->net_readonly &&
        !app::IsUncServerRoot(tab->current_path);  // \\server lists shares, not files
}

// Paste also reaches a phone folder (the shell host drops the clipboard on it).
bool CanPasteHere(const app::Tab* tab) {
    return CanCreateHere(tab) ||
        (IsDeviceTab(tab) && app::DeviceRegistry::Instance().FolderWritable(tab->current_path));
}

std::wstring JoinChild(std::wstring dir, const std::wstring& name) {
    if (!dir.ends_with(L"\\")) dir += L"\\";
    return dir + name;
}
} // namespace

// "新建 <type>.<ext>" from a template / data / nothing, then rename in place —
// the same ops path and rename intent as 新建文本文档.
void CreateNewFileItem(AppState& s, const std::wstring& stem, const std::wstring& ext,
                       std::wstring template_file, std::vector<uint8_t> data) {
    app::Tab* tab = ActiveTab(s);
    if (!CanCreateHere(tab)) return;
    ops::OpRequest req;
    req.type = ops::OpType::CreateTextFile;
    req.sources.push_back(JoinChild(tab->current_path, app::UniqueChildName(tab->current_path, stem, ext)));
    req.template_file = std::move(template_file);
    req.template_data = std::move(data);
    const auto task_id = s.ops.Submit(std::move(req));
    app::QueueCreateRenameIntent(tab->create_rename_intents, task_id,
        tab->current_path, tab->view_generation);
}

void CreateNewMarkdown(AppState& s) {
    CreateNewFileItem(s, TagLabel(l10n::StringId::NewItemNamed, l10n::Get(l10n::StringId::MarkdownDocument)),
                      L".md", {}, {});
}

namespace {
// Selected rows that are real items of this folder (not change-history rows).
std::vector<int> GroupableSelection(const app::Tab& tab) {
    std::vector<int> rows;
    if (!tab.snapshot) return rows;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) continue;
        if (tab.EntryAt(static_cast<size_t>(index)).change_record_only) continue;
        rows.push_back(index);
    }
    return rows;
}

bool CanGroupSelection(const app::Tab* tab) {
    return CanCreateHere(tab) && !IsRecycleTab(tab) && GroupableSelection(*tab).size() >= 2;
}

// Only plain folders dissolve: emptying a junction or symlink would move the
// items out of the folder it points at.
bool CanDissolveSelection(const app::Tab* tab) {
    if (!CanCreateHere(tab) || IsRecycleTab(tab)) return false;
    const std::vector<int> rows = GroupableSelection(*tab);
    if (rows.empty() || rows.size() != tab->SelectedIndices().size()) return false;
    for (int index : rows) {
        const fs::DirEntry& e = tab->EntryAt(static_cast<size_t>(index));
        if (!e.is_dir || e.is_reparse) return false;
    }
    return true;
}

// Ties the operations of one menu action into one undo step.
uint64_t NewUndoGroup() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

// Names directly inside dir; false when it cannot be listed.
bool ListFolderChildren(const std::wstring& dir, std::vector<std::wstring>& names) {
    names.clear();
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = JoinChild(dir, L"*");
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (std::wcscmp(data.cFileName, L".") != 0 && std::wcscmp(data.cFileName, L"..") != 0)
            names.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
}
} // namespace

// 合并到文件夹 (macOS "New Folder with Selection"): a new folder beside the
// selection, the selection moved into it, then its name is edited in place.
// Undo takes the items back out (and, one step later, the folder away).
void GroupSelectionIntoFolder(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!CanGroupSelection(tab)) return;
    std::vector<std::wstring> sources;
    for (int index : GroupableSelection(*tab)) sources.push_back(EntryFullPath(*tab, index));
    const std::wstring folder = JoinChild(tab->current_path,
        app::UniqueChildName(tab->current_path, l10n::Get(l10n::StringId::NewFolder), L""));
    const uint64_t group = NewUndoGroup();
    const std::wstring label = l10n::Get(l10n::StringId::NewFolderWithSelection).c_str();
    ops::OpRequest create;
    create.type = ops::OpType::CreateFolder;
    create.sources.push_back(folder);
    create.undo_group = group;
    create.undo_label = label;
    s.ops.Submit(std::move(create));
    // Operations run one at a time, so the move starts after the folder exists.
    ops::OpRequest move;
    move.type = ops::OpType::Move;
    move.sources = std::move(sources);
    move.dest_dir = folder;
    move.collision_policy = ops::CollisionPolicy::KeepBoth;
    move.undo_group = group;
    move.undo_label = label;
    const auto move_id = s.ops.Submit(std::move(move));
    s.groupRenameFolder = folder;
    s.groupRenameUndo = group;
    app::QueueCreateRenameIntent(tab->create_rename_intents, move_id,
        tab->current_path, tab->view_generation);
}

// 解散文件夹: each selected folder's contents move up into this folder (a
// clashing name becomes "name (2)"), then the emptied folder goes to the
// Recycle Bin. Unwrapping X\X first renames the outer X out of the way, so the
// inner one keeps the name. Undo restores the folder, then moves items back.
void DissolveSelectedFolders(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!CanDissolveSelection(tab)) return;
    std::vector<std::wstring> folders;
    for (int index : GroupableSelection(*tab)) folders.push_back(EntryFullPath(*tab, index));
    DissolveFolders(s, *tab, folders, NewUndoGroup(), l10n::Get(l10n::StringId::UngroupFolder).c_str());
}

// Each folder's items move up into its parent; when that is tab's folder they
// are selected there. The assistant's file.dissolve passes its own undo group
// and label (P4c) and waits for the returned tasks.
std::vector<uint64_t> DissolveFolders(AppState& s, app::Tab& tab_ref, const std::vector<std::wstring>& folders,
                                      uint64_t group, const std::wstring& label) {
    app::Tab* tab = &tab_ref;
    std::vector<uint64_t> tasks;
    std::vector<std::wstring> select;
    std::vector<std::wstring> children;
    bool in_tab = false;
    for (const std::wstring& folder : folders) {
        // Each folder's own parent (the assistant's items need not be in tab).
        const size_t cut = folder.find_last_of(L"\\/");
        if (cut == std::wstring::npos) continue;
        std::wstring parent = folder.substr(0, cut);
        if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
        const bool here = _wcsicmp(parent.c_str(), tab->current_path.c_str()) == 0;
        in_tab = in_tab || here;
        if (!ListFolderChildren(folder, children)) continue;
        if (children.empty()) {
            ops::OpRequest recycle;
            recycle.type = ops::OpType::RecycleDelete;
            recycle.sources.push_back(folder);
            recycle.undo_group = group;
            recycle.undo_label = label;
            const auto recycle_id = s.ops.Submit(std::move(recycle));
            tasks.push_back(recycle_id);
            s.dissolveSteps.push_back({recycle_id, parent});
            continue;
        }
        const size_t slash = folder.find_last_of(L"\\/");
        const std::wstring leaf = slash == std::wstring::npos ? folder : folder.substr(slash + 1);
        std::wstring container = folder;
        const bool clash = std::any_of(children.begin(), children.end(), [&](const std::wstring& name) {
            return _wcsicmp(name.c_str(), leaf.c_str()) == 0;
        });
        if (clash) {
            const std::wstring aside = app::UniqueChildName(parent, leaf, L"");
            ops::OpRequest rename;
            rename.type = ops::OpType::Rename;
            rename.sources.push_back(folder);
            rename.new_name = aside;
            rename.undo_group = group;
            rename.undo_label = label;
            tasks.push_back(s.ops.Submit(std::move(rename)));
            container = JoinChild(parent, aside);
        }
        ops::OpRequest move;
        move.type = ops::OpType::Move;
        for (const auto& name : children) {
            move.sources.push_back(JoinChild(container, name));
            if (here) select.push_back(name);
        }
        move.dest_dir = parent;
        move.collision_policy = ops::CollisionPolicy::KeepBoth;
        move.undo_group = group;
        move.undo_label = label;
        tasks.push_back(s.ops.Submit(std::move(move)));
        // Queued with the move so one undo always sees the whole action; a
        // child that could not move out keeps its folder.
        ops::OpRequest recycle;
        recycle.type = ops::OpType::RecycleDelete;
        recycle.sources.push_back(container);
        recycle.only_if_empty = true;
        recycle.undo_group = group;
        recycle.undo_label = label;
        const auto recycle_id = s.ops.Submit(std::move(recycle));
        tasks.push_back(recycle_id);
        s.dissolveSteps.push_back({recycle_id, parent});
    }
    if (in_tab && !select.empty()) {
        tab->pending_selected_name = select.front();
        tab->pending_selected_names = std::move(select);
        tab->pending_selection_revision = tab->selection_revision;
    }
    return tasks;
}

void QueueTagAds(AppState& s, std::vector<app::TagAdsUpdate> updates) {
    const HWND notify = ServiceWindow(s);
    app::QueueTagAdsDrain(std::move(updates), [&worker = s.worker](std::function<void()> task) {
        worker.EnqueueSerialIo(std::move(task));
    }, [notify](std::vector<app::TagAdsFailure> failures) {
        static std::atomic<uint64_t> revision{0};
        auto result = std::make_unique<app::TagAdsResult>();
        result->failures = std::move(failures);
        result->revision = ++revision;
        if (notify && PostMessageW(notify, WM_TAG_ADS_WARNING, 0,
                reinterpret_cast<LPARAM>(result.get()))) result.release();
    });
}

std::vector<app::TagAdsUpdate> BuildTagAdsUpdates(
        const app::PlacesCatalog& places, const std::vector<std::wstring>& paths,
        bool include_descendants) {
    std::vector<app::TagAdsUpdate> updates;
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring> candidates = paths;
    if (include_descendants) {
        auto below = [](const std::wstring& candidate, const std::wstring& root) {
            if (_wcsicmp(candidate.c_str(), root.c_str()) == 0) return true;
            if (candidate.size() <= root.size() ||
                _wcsnicmp(candidate.c_str(), root.c_str(), root.size()) != 0) return false;
            return root.ends_with(L"\\") || candidate[root.size()] == L'\\' ||
                   candidate[root.size()] == L'/';
        };
        for (const auto& tag : places.tags) {
            for (const auto& assigned : tag.paths) {
                if (std::any_of(paths.begin(), paths.end(),
                                [&](const std::wstring& root) { return below(assigned, root); })) {
                    candidates.push_back(assigned);
                }
            }
        }
    }
    updates.reserve(candidates.size());
    for (const auto& path : candidates) {
        const std::wstring key = TagDiscoveryKey(path);
        if (key.empty() || !seen.insert(key).second) continue;
        app::TagAdsUpdate update;
        update.path = path;
        for (int index : places.TagsForPath(path)) {
            if (index < 0 || index >= static_cast<int>(places.tags.size())) continue;
            const auto& tag = places.tags[static_cast<size_t>(index)];
            update.tag_names.push_back(tag.name);
            update.tags.push_back({ tag.id, tag.name, tag.rgb });
        }
        updates.push_back(std::move(update));
    }
    return updates;
}

bool ToggleTagForSelection(AppState& s, const app::TagId& tag_id,
                                  const std::vector<std::wstring>& paths) {
    if (tag_id.empty() || paths.empty()) return false;
    if (const app::Tab* tab = ActiveTab(s); tab && tab->SelectedCount() > 0 &&
        SelectedFullPaths(*tab) == paths && !app::ComputeSelectionCaps(*tab, &s.places).tags)
        return false;
    const bool add = s.places.GetSelectionState(tag_id, paths) != app::TagSelectionState::All;
    std::vector<app::TagAdsUpdate> updates;
    if (!s.places.SetTagsBatch(tag_id, paths, add, &updates)) return false;
    QueueTagAds(s, std::move(updates));
    if (add) {
        // The tag panel's 最近用过: newest first, five at most.
        auto& recent = s.appPrefs.recent_tags;
        std::erase(recent, tag_id);
        recent.insert(recent.begin(), tag_id);
        if (recent.size() > 5) recent.resize(5);
        s.appPrefs.Save();
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool ClearTagsForSelection(AppState& s, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    if (const app::Tab* tab = ActiveTab(s); tab && tab->SelectedCount() > 0 &&
        SelectedFullPaths(*tab) == paths && !app::ComputeSelectionCaps(*tab, &s.places).tags)
        return false;
    std::vector<app::TagId> present;
    for (const auto& path : paths) {
        for (int index : s.places.TagsForPath(path)) {
            const app::TagId& id = s.places.tags[static_cast<size_t>(index)].id;
            if (std::find(present.begin(), present.end(), id) == present.end()) present.push_back(id);
        }
    }
    std::vector<app::TagAdsUpdate> updates;
    bool changed = false;
    for (const auto& id : present) changed |= s.places.SetTagsBatch(id, paths, false, &updates);
    if (!changed) return false;
    QueueTagAds(s, std::move(updates));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

// Finder-style tag colors: the seven defaults plus any custom colors the
// user added through the color dialog (persisted in app prefs, shown last).
std::vector<uint32_t>& TagColorPalette(AppState& s) {
    static std::vector<uint32_t> palette;
    if (palette.empty()) {
        palette = { 0xEF4444, 0xF59E0B, 0xEAB308, 0x22C55E, 0x3B82F6, 0xA855F7,
                    0x94A3B8 };
        for (uint32_t c : s.appPrefs.custom_tag_colors) {
            if (std::find(palette.begin(), palette.end(), c) == palette.end())
                palette.push_back(c);
        }
    }
    return palette;
}

// Keeps the eight most recent custom colors.
void AppendCustomTagColor(AppState& s, uint32_t rgb) {
    constexpr size_t kMaxCustomTagColors = 8;
    std::vector<uint32_t>& palette = TagColorPalette(s);
    if (std::find(palette.begin(), palette.end(), rgb) != palette.end()) return;
    auto& custom = s.appPrefs.custom_tag_colors;
    custom.push_back(rgb);
    if (custom.size() > kMaxCustomTagColors)
        custom.erase(custom.begin(), custom.end() - static_cast<std::ptrdiff_t>(kMaxCustomTagColors));
    palette.clear();
    TagColorPalette(s);
    s.appPrefs.Save();
}

void ShowTagPicker(AppState& s, POINT screen_pt, const std::vector<std::wstring>* paths_override) {
    if (!paths_override &&
        DeferContentSelection(s,[=](AppState& v){ShowTagPicker(v,screen_pt);})) return;
    if (!paths_override) {
        const app::Tab* tab = ActiveTab(s);
        if (tab && tab->SelectedCount() > 0 && !app::ComputeSelectionCaps(*tab, &s.places).tags) return;
    }
    if (!EnsureMenu(s)) return;
    const std::vector<std::wstring> paths = paths_override ? *paths_override
        : ActiveTab(s) ? SelectedFullPaths(*ActiveTab(s)) : std::vector<std::wstring>{};
    constexpr int kTagPickerBase = 20000;
    constexpr int kCreateTag = app::kCreateTagCommand;
    // The tag panel (pulse_tag_redesign §6): "给 3 项打标签"; typing filters
    // and, without an exact match, offers 新建 first; otherwise 最近用过 →
    // 常用 (1-9, also typed into the empty filter) → each group. Each row is
    // three-state, a partial one showing "2/3".
    auto rebuild = [&s, &paths](const std::wstring& query) {
        std::vector<ui::FluentMenuItem> items;
        std::wstring needle = query;
        for (auto& c : needle) c = static_cast<wchar_t>(std::towlower(c));
        std::vector<int> counts(s.places.tags.size(), 0);
        for (const auto& path : paths) {
            for (int index : s.places.TagsForPath(path)) {
                if (index >= 0 && static_cast<size_t>(index) < counts.size()) ++counts[static_cast<size_t>(index)];
            }
        }
        const int total = static_cast<int>(paths.size());
        auto row = [&](int i) {
            const auto& tag = s.places.tags[static_cast<size_t>(i)];
            ui::FluentMenuItem item;
            item.command = kTagPickerBase + i;
            item.text = tag.name;
            item.has_swatch = true;
            item.swatch_color = ui::HexColor(tag.rgb);
            const int count = counts[static_cast<size_t>(i)];
            item.checked = total > 0 && count == total;
            item.mixed = count > 0 && count < total;
            if (item.mixed) item.shortcut = std::to_wstring(count) + L"/" + std::to_wstring(total);
            if (tag.favorite > 0) {
                if (!item.shortcut.empty()) item.shortcut += L"   ";
                item.shortcut += std::to_wstring(tag.favorite);
            }
            return item;
        };
        auto heading = [&items](const std::wstring& text) {
            if (!items.empty()) items.back().separator_after = true;
            ui::FluentMenuItem item;
            item.text = text;
            item.enabled = false;
            item.secondary = true;
            items.push_back(std::move(item));
        };
        if (total > 0) heading(TagLabel(l10n::StringId::TagPanelHeaderFormat, std::to_wstring(total)));
        if (!needle.empty()) {
            bool exact = false;
            std::vector<ui::FluentMenuItem> matches;
            for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
                std::wstring lower = s.places.tags[static_cast<size_t>(i)].name;
                for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
                if (lower.find(needle) == std::wstring::npos) continue;
                exact = exact || lower == needle;
                matches.push_back(row(i));
            }
            if (!items.empty()) items.back().separator_after = true;
            if (!exact) {
                ui::FluentMenuItem create;
                create.command = kCreateTag;
                create.text = TagLabel(l10n::StringId::TagCreateFormat, query);
                create.glyph = L"\xE710";
                create.separator_after = !matches.empty();
                items.push_back(std::move(create));
            }
            items.insert(items.end(), std::make_move_iterator(matches.begin()),
                         std::make_move_iterator(matches.end()));
            return items;
        }
        std::vector<char> shown(s.places.tags.size(), 0);
        bool recent_heading = false;
        for (const auto& id : s.appPrefs.recent_tags) {
            const int i = s.places.FindTagIndex(id);
            if (i < 0 || shown[static_cast<size_t>(i)]) continue;
            if (!recent_heading) heading(l10n::Get(l10n::StringId::TagPanelRecent));
            recent_heading = true;
            shown[static_cast<size_t>(i)] = 1;
            items.push_back(row(i));
        }
        bool favorite_heading = false;
        for (int slot = 1; slot <= app::kMaxFavoriteTags; ++slot) {
            const int i = s.places.FavoriteTagIndex(slot);
            if (i < 0 || shown[static_cast<size_t>(i)]) continue;
            if (!favorite_heading) heading(l10n::Get(l10n::StringId::TagPanelFavorites));
            favorite_heading = true;
            shown[static_cast<size_t>(i)] = 1;
            items.push_back(row(i));
        }
        // The rest under their groups, the ungrouped ones last.
        std::vector<std::wstring> groups;
        for (const auto& group : s.places.tag_groups) groups.push_back(group.id);
        groups.push_back(std::wstring{});
        for (const auto& group_id : groups) {
            bool group_heading = false;
            for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
                if (shown[static_cast<size_t>(i)] ||
                    s.places.TagGroupOf(s.places.tags[static_cast<size_t>(i)]) != group_id) continue;
                if (!group_heading && (recent_heading || favorite_heading || !s.places.tag_groups.empty())) {
                    const app::TagGroup* group = group_id.empty() ? nullptr : s.places.FindTagGroup(group_id);
                    heading(group ? group->name : l10n::Get(l10n::StringId::TagUngrouped));
                }
                group_heading = true;
                shown[static_cast<size_t>(i)] = 1;
                items.push_back(row(i));
            }
        }
        return items;
    };
    for (;;) {
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TagSearchHint));
        s.menu->SetFilterMinWidth(300.0f);
        std::vector<std::pair<wchar_t, int>> digit_keys;
        for (int slot = 1; slot <= app::kMaxFavoriteTags; ++slot) {
            const int index = s.places.FavoriteTagIndex(slot);
            if (index >= 0) digit_keys.emplace_back(static_cast<wchar_t>(L'0' + slot), kTagPickerBase + index);
        }
        s.menu->SetEmptyFilterKeys(std::move(digit_keys));
        int command = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
        const std::wstring query = s.menu->LastFilterQuery();
        if (command == app::CmdNone) {
            // Enter with no highlighted row commits the typed name: toggle an
            // exact match, otherwise fall through to the create branch.
            if (!s.menu->LastFilterCommitted() || query.empty()) break;
            command = kCreateTag;
            std::wstring lower = query;
            for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
            for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
                std::wstring name = s.places.tags[static_cast<size_t>(i)].name;
                for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
                if (name == lower) { command = kTagPickerBase + i; break; }
            }
        }

        app::TagId tag_id;
        if (command == kCreateTag) {
            const std::vector<uint32_t>& palette = TagColorPalette(s);
            tag_id = s.places.CreateTag(query,
                palette[s.places.tags.size() % palette.size()]);
        } else if (command >= kTagPickerBase &&
                   command < kTagPickerBase + static_cast<int>(s.places.tags.size())) {
            tag_id = s.places.tags[static_cast<size_t>(command - kTagPickerBase)].id;
        }
        if (!tag_id.empty()) ToggleTagForSelection(s, tag_id, paths);
    }
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
}

// Hands the files to the shell's "Send to > Compressed (zipped) folder"
// target; Windows names the archive after the first item and writes it next
// to it, with its own progress UI.
static bool ZipViaShell(const std::vector<std::wstring>& paths) {
    PWSTR sendto = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &sendto)) || !sendto) return false;
    const std::wstring dir = sendto;
    CoTaskMemFree(sendto);
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.ZFSendToTarget").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    const std::wstring target = dir + L"\\" + fd.cFileName;
    FindClose(find);
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item)))) return false;
    IDropTarget* drop = nullptr;
    const HRESULT hr = item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&drop));
    item->Release();
    if (FAILED(hr) || !drop) return false;
    ui::FileDataObject* data = ui::FileDataObject::Create(paths);
    bool ok = false;
    if (data) {
        POINTL pt{ 0, 0 };
        DWORD effect = DROPEFFECT_COPY;
        if (SUCCEEDED(drop->DragEnter(data, MK_LBUTTON, pt, &effect)) && effect != DROPEFFECT_NONE) {
            effect = DROPEFFECT_COPY;
            ok = SUCCEEDED(drop->Drop(data, MK_LBUTTON, pt, &effect));
        } else {
            drop->DragLeave();
        }
        data->Release();
    }
    drop->Release();
    return ok;
}

void TrayFindStale(AppState& s) {
    for (const auto& batch : s.tray.batches()) {
        for (const auto& item : batch.items) {
            if (item.exists) continue;
            const size_t slash = item.path.find_last_of(L"\\/");
            const std::wstring name = slash == std::wstring::npos ? item.path : item.path.substr(slash + 1);
            if (name.empty()) continue;
            app::AdvancedSearchSpec spec = app::ParseSearchQuery(name);
            spec.location = app::LocationScope::Indexed;
            spec.current_folder.clear();
            spec.custom_folder.clear();
            NavigateTo(s, app::MakeSearchPath(app::CompileSearchQuery(spec)));
            return;
        }
    }
}

void TrayRemoveStale(AppState& s) {
    if (s.tray.RemoveMissing() > 0) SpawnTrayPuffs(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void TrayToggleCompare(AppState& s) {
    s.trayCompare = !s.trayCompare;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static int CompareFileBytes(const std::wstring& a, const std::wstring& b) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    int result = 4;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE) {
        constexpr DWORD kChunk = 1u << 20;
        std::vector<char> ba(kChunk), bb(kChunk);
        for (;;) {
            DWORD ra = 0, rb = 0;
            if (!ReadFile(ha, ba.data(), kChunk, &ra, nullptr) ||
                !ReadFile(hb, bb.data(), kChunk, &rb, nullptr)) {
                result = 4;
                break;
            }
            if (ra != rb || std::memcmp(ba.data(), bb.data(), ra) != 0) {
                result = 3;
                break;
            }
            if (ra == 0) {
                result = 2;
                break;
            }
        }
    }
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return result;
}

// The pair the compare views show: the two checked items of the open view,
// else the whole tray when it holds exactly two.
static std::vector<std::wstring> TrayComparePaths(const AppState& s) {
    std::vector<std::wstring> paths;
    if (s.trayView.compare && s.trayView.checked.size() == 2) {
        for (const uint64_t id : s.trayView.checked)
            for (const auto& batch : s.tray.batches())
                for (const auto& item : batch.items)
                    if (item.id == id) paths.push_back(item.path);
        return paths;
    }
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) paths.push_back(item.path);
    return paths;
}

void TrayStartContentCompare(AppState& s) {
    const std::vector<std::wstring> paths = TrayComparePaths(s);
    if (paths.size() != 2) return;
    auto job = std::make_shared<TrayCompareJob>();
    job->a = paths[0];
    job->b = paths[1];
    s.trayCmpJob = job;
    const HWND hwnd = s.hwnd;
    std::thread([job, hwnd] {
        job->state.store(CompareFileBytes(job->a, job->b));
        InvalidateRect(hwnd, nullptr, FALSE);
    }).detach();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void TrayOpenTextDiff(AppState& s) {
    const std::vector<std::wstring> paths = TrayComparePaths(s);
    if (paths.size() != 2) return;
    if (!s.textDiff) s.textDiff = std::make_unique<ui::TextDiffWindow>();
    s.textDiff->Show(s.hwnd, paths[0], paths[1], s.darkMode, s.accentColor);
}

// Shelf ⋯ menu (pulse_tray_redesign.html moreMenu); also the tray's
// right-click menu. ZIP and rename are shelf buttons now.
void ShowTrayBatchMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const std::vector<std::wstring> paths = TrayLivePaths(s);
    enum { kTag = 1, kCopyPaths, kOpenAll, kReveal, kClear, kDiff };
    std::vector<ui::FluentMenuItem> items;
    auto add = [&](int cmd, l10n::StringId id, const wchar_t* glyph, bool enabled, bool sep) {
        ui::FluentMenuItem item;
        item.command = cmd;
        item.text = l10n::Get(id);
        item.glyph = glyph;
        item.enabled = enabled;
        item.separator_after = sep;
        items.push_back(std::move(item));
    };
    const bool any = !paths.empty();
    size_t staged = 0;
    for (const auto& batch : s.tray.batches()) staged += batch.items.size();
    const bool two_text = paths.size() == 2 && staged == 2 &&
                          diff::ProbeLooksText(paths[0]) && diff::ProbeLooksText(paths[1]);
    add(kCopyPaths, l10n::StringId::TrayMenuCopyPaths, L"\xE8C8", any, false);
    add(kTag, l10n::StringId::TrayMenuTag, L"\xE8EC", any, false);
    add(kOpenAll, l10n::StringId::TrayMenuOpenAll, L"\xE8E5", any, false);
    add(kReveal, l10n::StringId::TrayMenuReveal, L"\xE838", any, false);
    add(kDiff, l10n::StringId::TrayCompareTwo, L"\xE8AB", two_text, true);
    add(kClear, l10n::StringId::TrayMenuClear, L"\xE74D", !s.tray.batches().empty(), false);
    // The tooltip would otherwise linger under the menu.
    s.tooltipText.clear();
    s.hoverSince = 0;
    s.hoverRegion = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    UpdateWindow(s.hwnd);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(items));
    switch (cmd) {
    case kDiff:
        TrayOpenTextDiff(s);
        break;
    case kTag:
        ShowTagPicker(s, screen_pt, &paths);
        break;
    case kCopyPaths: {
        std::wstring text;
        for (const auto& p : paths) {
            if (!text.empty()) text += L"\r\n";
            text += ClipboardPath(p);
        }
        ops::WriteClipboardText(text);
        break;
    }
    case kOpenAll: {
        if (paths.size() > 10) {
            ui::ConfirmDialogSpec spec;
            spec.title = l10n::Get(l10n::StringId::TrayMenuOpenAll);
            spec.message = l10n::Get(l10n::StringId::TrayOpenAllConfirm);
            const size_t at = spec.message.find(L"{n}");
            if (at != std::wstring::npos) spec.message.replace(at, 3, std::to_wstring(paths.size()));
            spec.confirm_text = l10n::Get(l10n::StringId::TrayMenuOpenAll);
            spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
            if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) break;
        }
        // The same open service as the list: long-path prefixes, the item's
        // folder as working directory, and off the UI thread.
        for (const auto& p : paths) s.ops.OpenWith(p);
        break;
    }
    case kReveal: {
        // Open the first item's folder and select every staged item living there.
        const std::wstring parent = fs::ParentPath(paths.front());
        if (parent.empty()) break;
        std::vector<std::wstring> leaves;
        for (const auto& p : paths) {
            if (_wcsicmp(fs::ParentPath(p).c_str(), parent.c_str()) != 0) continue;
            const size_t slash = p.find_last_of(L"\\/");
            leaves.push_back(slash == std::wstring::npos ? p : p.substr(slash + 1));
        }
        NavigateTo(s, parent);
        if (app::Tab* tab = ActiveTab(s)) {
            tab->pending_selected_names = leaves;
            tab->pending_selection_revision = tab->selection_revision;
            if (!leaves.empty()) tab->pending_selected_name = leaves.front();
            tab->pending_ensure_selection_visible = true;
        }
        break;
    }
    case kClear:
        ClearTrayWithUndo(s);
        break;
    default:
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Shelf ▾ menu: 放置方式 (the tray-wide mode) and 送到 targets that skip
// opening the folder, then 选择文件夹….
void ShowTrayReleaseMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    enum { kCopy = 1, kMove, kPick, kDestBase = 100 };
    std::vector<std::wstring> dests;
    std::wstring current;
    if (const app::Tab* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path))
        current = ClipboardPath(fs::NormalizePath(tab->current_path));
    for (const auto& d : TrayDestList(s))
        if (current.empty() || _wcsicmp(d.c_str(), current.c_str()) != 0) dests.push_back(d);
    std::vector<ui::FluentMenuItem> items;
    auto header = [&](l10n::StringId id) {
        ui::FluentMenuItem item;
        item.text = l10n::Get(id);
        item.enabled = false;
        item.secondary = true;
        items.push_back(std::move(item));
    };
    header(l10n::StringId::TrayMenuPlacement);
    {
        ui::FluentMenuItem copy;
        copy.command = kCopy;
        copy.text = l10n::Get(l10n::StringId::TrayReleaseCopy);
        copy.shortcut = l10n::Get(l10n::StringId::TrayDropDefault);
        copy.radio_group = true;
        copy.radio = !s.trayMoveMode;
        items.push_back(std::move(copy));
        ui::FluentMenuItem move;
        move.command = kMove;
        move.text = l10n::Get(l10n::StringId::TrayReleaseMove);
        move.shortcut = L"Shift";
        move.radio_group = true;
        move.radio = s.trayMoveMode;
        move.separator_after = true;
        items.push_back(std::move(move));
    }
    header(l10n::StringId::TrayMenuSendHeader);
    for (size_t i = 0; i < dests.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = kDestBase + static_cast<int>(i);
        std::wstring label = dests[i];
        while (label.size() > 3 && label.back() == L'\\') label.pop_back();
        const size_t slash = label.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < label.size()) label = label.substr(slash + 1);
        item.text = label;
        item.glyph = L"\xE8B7";
        item.tooltip = dests[i];
        item.enabled = fs::IsUncPath(dests[i]) || GetFileAttributesW(dests[i].c_str()) != INVALID_FILE_ATTRIBUTES;
        items.push_back(std::move(item));
    }
    {
        ui::FluentMenuItem pick;
        pick.command = kPick;
        pick.text = l10n::Get(l10n::StringId::TrayPickFolder);
        pick.glyph = L"\xE838";
        items.push_back(std::move(pick));
    }
    s.tooltipText.clear();
    s.hoverSince = 0;
    s.hoverRegion = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    UpdateWindow(s.hwnd);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const bool move_now = TrayEffectiveMove(s);
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(items));
    if (cmd == kCopy || cmd == kMove) {
        s.trayMoveMode = cmd == kMove;
    } else if (cmd == kPick) {
        std::wstring folder;
        if (PickFolder(s, folder, l10n::Get(l10n::StringId::TrayPickFolder).c_str(), L"tray_send") && !folder.empty())
            TraySendTo(s, folder, move_now);
    } else if (cmd >= kDestBase && cmd < kDestBase + static_cast<int>(dests.size())) {
        TraySendTo(s, dests[static_cast<size_t>(cmd - kDestBase)], move_now);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// 送到: release into a folder without opening it; its chip flashes.
void TraySendTo(AppState& s, const std::wstring& dir, bool move) {
    ReleaseTrayTo(s, dir, move);
    s.trayShelf.flash_path = ClipboardPath(fs::NormalizePath(dir));
    s.trayShelf.flash_at = ui::motion::NowMs();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Shelf ZIP: "暂存 N 项.zip" in the current folder, ring on the button.
// Without the built-in compressor the shell's zipped-folder target does it.
void TrayShelfZip(AppState& s) {
    if (s.trayZip.id) return;
    const app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    std::vector<std::wstring> paths = TrayLivePaths(s);
    if (paths.empty()) return;
    std::wstring base = l10n::Get(l10n::StringId::TrayZipName);
    if (const size_t at = base.find(L"{n}"); at != std::wstring::npos) base.replace(at, 3, std::to_wstring(paths.size()));
    auto progress = std::make_shared<std::atomic<float>>(0.0f);
    const uint64_t id = CompressPathsTo(s, std::move(paths), tab->current_path, base, progress);
    if (!id) {
        ZipViaShell(TrayLivePaths(s));
        return;
    }
    s.trayZip.id = id;
    s.trayZip.progress = std::move(progress);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

#ifdef PULSE_WITH_SELFTEST
static thread_local CreateTagPickerTestIo* create_tag_test_io = nullptr;
CreateTagPickerTestIo* SetCreateTagPickerTestIo(CreateTagPickerTestIo* io) {
    auto* previous = create_tag_test_io; create_tag_test_io = io; return previous;
}
#endif
void ShowCreateTagPicker(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    constexpr int kCreateTag = app::kCreateTagCommand;
    constexpr int kColorStrip = app::kTagColorStripCommand;
    constexpr int kColorBase = app::kTagColorBaseCommand;
    constexpr int kCustomColor = app::kCustomTagColorCommand;
    uint32_t selected_rgb = TagColorPalette(s)[0];
    auto rebuild = [&](const std::wstring& query) {
        const std::vector<uint32_t>& palette = TagColorPalette(s);
        ui::FluentMenuItem colors;
        colors.command = kColorStrip;
        colors.separator_after = true;
        colors.quick_swatches.reserve(palette.size());
        for (size_t i = 0; i < std::min(palette.size(), app::kMaxTagColorCommands); ++i) {
            ui::FluentMenuSwatch swatch;
            swatch.command = kColorBase + static_cast<int>(i);
            swatch.color = ui::HexColor(palette[static_cast<size_t>(i)]);
            swatch.checked = selected_rgb == palette[static_cast<size_t>(i)];
            colors.quick_swatches.push_back(std::move(swatch));
        }
        std::vector<ui::FluentMenuItem> items;
        items.push_back(std::move(colors));
        ui::FluentMenuItem create;
        create.command = kCreateTag;
        create.text = query.empty() ? l10n::Get(l10n::StringId::TagNameHint)
                                    : TagLabel(l10n::StringId::TagCreateFormat, query);
        create.glyph = L"\xE710";
        create.enabled = !query.empty();
        items.push_back(std::move(create));
        ui::FluentMenuItem custom;
        custom.command = kCustomColor;
        custom.text = l10n::Get(l10n::StringId::CustomColor);
        custom.has_swatch = true;
        custom.swatch_color = ui::HexColor(selected_rgb);
        custom.separator_after = true;
        items.push_back(std::move(custom));
        return items;
    };
    std::wstring name;
    for (;;) {
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TagNewHint));
        s.menu->SetFilterMinWidth(300.0f);
        s.menu->SetInitialFilterText(name);
        int command;
        bool committed;
#ifdef PULSE_WITH_SELFTEST
        if (create_tag_test_io) {
            const auto choice = create_tag_test_io->menu(rebuild(name), rebuild);
            command = choice.command; name = choice.name; committed = choice.committed;
        } else
#endif
        {
            command = s.menu->TrackPopup(screen_pt, rebuild(name), rebuild);
            name = s.menu->LastFilterQuery(); committed = s.menu->LastFilterCommitted();
        }
        if (app::IsTagColorCommand(command, TagColorPalette(s).size())) {
            selected_rgb = TagColorPalette(s)[static_cast<size_t>(command - kColorBase)];
            continue;
        }
        if (command == kCustomColor) {
            uint32_t picked = selected_rgb;
            bool accepted;
#ifdef PULSE_WITH_SELFTEST
            if (create_tag_test_io) accepted = create_tag_test_io->color(picked);
            else
#endif
                accepted = ui::ColorPickerPopup::Pick(s.hwnd, &s.compositor, s.menu.get(), s.scale,
                                           screen_pt, picked, s.darkMode, picked);
            if (accepted) {
                selected_rgb = picked;
                // Accepted custom colors join the swatch strip (last dot).
                AppendCustomTagColor(s, picked);
            }
            continue;
        }
        if ((command == kCreateTag ||
             (command == app::CmdNone && committed)) && !name.empty()) {
            s.places.CreateTag(name, selected_rgb);
        }
        break;
    }
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// A one-field prompt in the menu (the tag rename pattern): the typed text, or
// empty when the menu was dismissed.
static std::wstring PromptTagText(AppState& s, POINT screen_pt, const std::wstring& placeholder,
                                  l10n::StringId format, l10n::StringId empty_hint) {
    constexpr int kApply = 31300;
    auto rebuild = [format, empty_hint](const std::wstring& query) {
        ui::FluentMenuItem item;
        item.command = kApply;
        item.text = query.empty() ? l10n::Get(empty_hint) : TagLabel(format, query);
        item.glyph = L"\xE8AC";
        item.enabled = !query.empty();
        return std::vector<ui::FluentMenuItem>{ std::move(item) };
    };
    s.menu->SetFilterPlaceholder(placeholder);
    s.menu->SetFilterMinWidth(300.0f);
    const int apply = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
    const std::wstring text = s.menu->LastFilterQuery();
    const bool committed = s.menu->LastFilterCommitted();
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    if (apply == kApply || (apply == app::CmdNone && committed)) return text;
    return {};
}

std::wstring PromptSmartViewName(AppState& s, POINT screen_pt, const std::wstring& suggested) {
    if (!EnsureMenu(s)) return {};
    return PromptTagText(s, screen_pt, suggested, l10n::StringId::TagSmartViewSaveFormat,
                         l10n::StringId::TagSmartViewNameHint);
}

std::wstring CreateTagGroupInteractive(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return {};
    const std::wstring name = PromptTagText(s, screen_pt, l10n::Get(l10n::StringId::TagGroupNameHint),
                                            l10n::StringId::TagGroupCreateFormat,
                                            l10n::StringId::TagGroupNameHint);
    if (name.empty()) return {};
    const std::wstring id = s.places.CreateTagGroup(name);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return id;
}

void ToggleTagGroupRow(AppState& s, const std::wstring& path) {
    const app::TagGroup* group = s.places.FindTagGroup(app::TagGroupIdFromPath(path));
    if (!group) return;
    s.places.SetTagGroupCollapsed(group->id, !group->collapsed);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowTagGroupMenu(AppState& s, const std::wstring& group_id, POINT screen_pt) {
    const app::TagGroup* group = s.places.FindTagGroup(group_id);
    if (!group || !EnsureMenu(s)) return;
    constexpr int kRenameGroup = 31500;
    constexpr int kDeleteGroup = 31501;
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem rename;
    rename.command = kRenameGroup;
    rename.text = l10n::Get(l10n::StringId::TagGroupRename);
    rename.glyph = L"\xE8AC";
    items.push_back(std::move(rename));
    ui::FluentMenuItem remove;
    remove.command = kDeleteGroup;
    remove.text = l10n::Get(l10n::StringId::TagGroupDelete);
    remove.glyph = L"\xE74D";
    items.push_back(std::move(remove));
    const std::wstring name = group->name;
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kRenameGroup) {
        const std::wstring renamed = PromptTagText(s, screen_pt, name, l10n::StringId::TagRenameFormat,
                                                   l10n::StringId::TagRenameHint);
        if (!renamed.empty()) s.places.RenameTagGroup(group_id, renamed);
    } else if (command == kDeleteGroup) {
        // Only the grouping goes; the tags stay, ungrouped.
        s.places.DeleteTagGroup(group_id);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool RenameTagInteractive(AppState& s, const app::TagId& tag_id, POINT screen_pt) {
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag || !EnsureMenu(s)) return false;
    // Enter without highlighting the row also confirms the typed name.
    const std::wstring name = PromptTagText(s, screen_pt, tag->name, l10n::StringId::TagRenameFormat,
                                            l10n::StringId::TagRenameHint);
    if (name.empty()) return false;
    const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
    if (!s.places.RenameTag(tag_id, name)) return false;
    QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
    return true;
}

void ShowTagSidebarMenu(AppState& s, const app::TagId& tag_id, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    constexpr int kRename = 31000;
    constexpr int kFavorite = 31010;
    constexpr int kNoGroup = 31020;
    constexpr int kNewGroup = 31021;
    constexpr int kGroupBase = 31400;
    constexpr int kCustomColor = 31090;
    constexpr int kColorBase = 31100;
    constexpr int kDelete = 31200;
    constexpr int kMissingClean = 31300;
    constexpr int kMissingRelocate = 31301;
    constexpr int kRuleNew = 31600;
    constexpr int kRuleBase = 31610;   // + n: the tag's n-th rule (click removes it)
    const std::vector<uint32_t>& palette = TagColorPalette(s);
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem rename;
    rename.command = kRename;
    rename.text = l10n::Get(l10n::StringId::TagRename);
    rename.glyph = L"\xE8AC";
    items.push_back(std::move(rename));
    // Favorites own the Alt+1..9 keys; a full set of nine leaves "add" off.
    ui::FluentMenuItem favorite;
    favorite.command = kFavorite;
    favorite.glyph = tag->favorite > 0 ? L"\xE735" : L"\xE734";
    if (tag->favorite > 0) {
        favorite.text = l10n::Get(l10n::StringId::TagFavoriteRemove);
        favorite.shortcut = app::ShownShortcut(L"Alt+" + std::to_wstring(tag->favorite));
    } else {
        favorite.text = l10n::Get(l10n::StringId::TagFavoriteAdd);
        int used = 0;
        for (const auto& other : s.places.tags) used += other.favorite > 0 ? 1 : 0;
        favorite.enabled = used < app::kMaxFavoriteTags;
    }
    items.push_back(std::move(favorite));
    // 移到分组 ▸: the groups (current one checked), 不分组, 新建分组…
    ui::FluentMenuItem move;
    move.text = l10n::Get(l10n::StringId::TagMoveToGroup);
    move.glyph = L"\xE8B7";
    const std::wstring current_group = s.places.TagGroupOf(*tag);
    for (int i = 0; i < static_cast<int>(s.places.tag_groups.size()) && i < 64; ++i) {
        const auto& group = s.places.tag_groups[static_cast<size_t>(i)];
        ui::FluentMenuItem item;
        item.command = kGroupBase + i;
        item.text = group.name;
        item.radio_group = true;
        item.radio = group.id == current_group;
        move.children.push_back(std::move(item));
    }
    ui::FluentMenuItem no_group;
    no_group.command = kNoGroup;
    no_group.text = l10n::Get(l10n::StringId::TagNoGroup);
    no_group.radio_group = true;
    no_group.radio = current_group.empty();
    no_group.separator_after = true;
    move.children.push_back(std::move(no_group));
    ui::FluentMenuItem new_group;
    new_group.command = kNewGroup;
    new_group.text = l10n::Get(l10n::StringId::TagNewGroup);
    new_group.glyph = L"\xE710";
    move.children.push_back(std::move(new_group));
    move.separator_after = true;
    items.push_back(std::move(move));
    bool preset_color = false;
    for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
        const uint32_t rgb = palette[static_cast<size_t>(i)];
        ui::FluentMenuItem color;
        color.command = kColorBase + i;
        if (i < 7) {
            constexpr l10n::StringId colors[] = {
                l10n::StringId::ColorRed, l10n::StringId::ColorOrange,
                l10n::StringId::ColorYellow, l10n::StringId::ColorGreen,
                l10n::StringId::ColorBlue, l10n::StringId::ColorPurple,
                l10n::StringId::ColorGray
            };
            color.text = l10n::Get(colors[i]);
        } else {
            wchar_t hex[8]{};
            swprintf_s(hex, L"#%06X", rgb);
            color.text = hex;
        }
        color.has_swatch = true;
        color.swatch_color = ui::HexColor(rgb);
        color.checked = tag->rgb == rgb;
        preset_color = preset_color || color.checked;
        items.push_back(std::move(color));
    }
    // Any color through the picker; accepted colors join the swatch list.
    ui::FluentMenuItem custom;
    custom.command = kCustomColor;
    custom.text = l10n::Get(l10n::StringId::CustomColor);
    custom.has_swatch = !preset_color;
    custom.swatch_color = ui::HexColor(tag->rgb);
    custom.glyph = preset_color ? L"\xE790" : L"";
    custom.separator_after = true;
    items.push_back(std::move(custom));
    AppendTagRulesMenu(*tag, items, kRuleNew, kRuleBase);
    items.back().separator_after = true;
    AppendMissingTagMenu(*tag, items, kMissingClean, kMissingRelocate);
    ui::FluentMenuItem remove;
    remove.command = kDelete;
    remove.text = l10n::Get(l10n::StringId::TagDelete);
    remove.glyph = L"\xE74D";
    items.push_back(std::move(remove));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kRename) {
        RenameTagInteractive(s, tag_id, screen_pt);
    } else if (command == kFavorite) {
        s.places.SetTagFavorite(tag_id, tag->favorite == 0);
    } else if (command == kNoGroup) {
        s.places.SetTagGroup(tag_id, {});
    } else if (command == kNewGroup) {
        const std::wstring group = CreateTagGroupInteractive(s, screen_pt);
        if (!group.empty()) s.places.SetTagGroup(tag_id, group);
    } else if (HandleTagRulesCommand(s, tag_id, command, kRuleNew, kRuleBase, screen_pt)) {
    } else if (command == kMissingClean) {
        CleanMissingTagged(s, {tag_id});
    } else if (command == kMissingRelocate) {
        RelocateMissingTagged(s, tag_id);
    } else if (command >= kGroupBase &&
               command < kGroupBase + static_cast<int>(s.places.tag_groups.size())) {
        s.places.SetTagGroup(tag_id, s.places.tag_groups[static_cast<size_t>(command - kGroupBase)].id);
    } else if (command == kCustomColor) {
        uint32_t picked = tag->rgb;
        if (ui::ColorPickerPopup::Pick(s.hwnd, &s.compositor, s.menu.get(), s.scale,
                                       screen_pt, picked, s.darkMode, picked)) {
            AppendCustomTagColor(s, picked);
            const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
            if (s.places.SetTagColor(tag_id, picked))
                QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
        }
    } else if (command >= kColorBase && command < kColorBase + static_cast<int>(palette.size())) {
        const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
        if (s.places.SetTagColor(tag_id, palette[static_cast<size_t>(command - kColorBase)]))
            QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
    } else if (command == kDelete) {
        const size_t count = s.places.PathsForTag(tag_id).size();
        ui::ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(l10n::StringId::TagDelete);
        confirm.confirm_text = l10n::Get(l10n::StringId::Delete);
        confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
        confirm.danger = true;
        confirm.message = count == 0
            ? TagLabel(l10n::StringId::TagDeleteConfirmFormat, tag->name)
            : TagLabel(l10n::StringId::TagDeleteUsedFormat, tag->name, count);
        if (ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) {
            DeleteTagAndRefreshViews(s, tag_id);
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool DeleteTagAndRefreshViews(AppState& s, const app::TagId& tag_id) {
    if (!s.places.FindTag(tag_id)) return false;
    const auto path = app::MakeTagPath(tag_id);
    std::vector<app::Tab*> affected;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab) return;
        std::wstring kind, reference;
        app::ParsePulsePath(tab->current_path, &kind, &reference);
        if (kind == L"tag" && s.places.ResolveTagRef(reference) == tag_id)
            affected.push_back(tab);
        else if (kind == L"tags" && app::ParseTagQuery(reference).Contains(tag_id))
            affected.push_back(tab);
    });
    std::vector<app::TagAdsUpdate> updates;
    s.places.DeleteTag(tag_id, &updates);
    QueueTagAds(s, std::move(updates));
    // Resolve legacy references before deletion changes the catalog indices.
    for (auto* tab : affected) {
        // A combined view drops the deleted term and keeps the others.
        LoadVirtualView(s, *tab, tab->current_path.starts_with(L"pulse:tags:") ? tab->current_path : path);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

// ---------------------------------------------------------------------------
// What the selection is (a drive, a share, a Recycle Bin item, ...) decides
// which commands it gets: toolbar, chip, item menu and shortcuts alike.
app::SelectionCaps SelectionCapsOf(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    return tab ? app::ComputeSelectionCaps(*tab, &s.places) : app::SelectionCaps{};
}

namespace {
struct SelectedDrive {
    std::wstring root;  // "H:\"
    uint8_t type = 0;
    bool locked = false;
};

std::vector<SelectedDrive> SelectedDrives(const app::Tab& tab) {
    std::vector<SelectedDrive> drives;
    // This PC rows, and the network page's mapped drives (网络驱动器).
    if ((!tab.current_path.empty() && tab.current_path != kNetworkLocationsPath) || !tab.snapshot) return drives;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
        const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(index));
        if (!e.drive_type) continue;
        std::wstring root = fs::DriveRootOf(EntryFullPath(tab, index));
        if (!root.empty()) drives.push_back({std::move(root), e.drive_type, e.drive_locked});
    }
    return drives;
}

std::wstring DriveDisplay(const std::wstring& root) {
    return root.size() >= 2 ? root.substr(0, 2) : root;
}

// The selected Pulse network pins (Network view).
std::vector<std::wstring> SelectedNetworkPins(AppState& s) {
    std::vector<std::wstring> pins;
    const app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path != kNetworkLocationsPath) return pins;
    for (const std::wstring& path : SelectedFullPaths(*tab))
        if (s.places.FindNetwork(path) >= 0) pins.push_back(path);
    return pins;
}

// The share a 映射网络驱动器 row maps: one selected \\server\share.
std::wstring SelectedShare(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || tab->SelectedCount() != 1) return {};
    const std::wstring path = SelectedFullPath(s);
    // WNetConnectionDialog1 requires a conventional \\server\\share path,
    // not Win32's internal \\?\\UNC\\ extended prefix.
    return app::IsUncShareRoot(path) ? ClipboardPath(path) : std::wstring{};
}

// 在终端打开 in This PC / a server / a Pulse view opens the selected folder;
// in a real folder it opens that folder, as before.
std::wstring TerminalTarget(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return {};
    const bool real_folder = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
        !app::IsUncServerRoot(tab->current_path);
    if (real_folder) return tab->current_path;
    if (tab->SelectedCount() != 1 || !tab->snapshot || tab->selected_index < 0 ||
        static_cast<size_t>(tab->selected_index) >= tab->EntryCount())
        return {};
    const fs::DirEntry e = tab->EntryAt(static_cast<size_t>(tab->selected_index));
    if (!e.is_dir || e.drive_locked) return {};
    std::wstring path = EntryFullPath(*tab, tab->selected_index);
    return fs::IsVirtualPath(path) ? std::wstring{} : path;
}

void RefreshAfterDriveChange(AppState& s) {
    RefreshPath(s, L"", RefreshReason::Explicit);  // This PC
    RequestSidebarRefresh(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void RunMapNetworkDrive(AppState& s, const std::wstring& share) {
    const DWORD error = app::MapNetworkDrive(s.hwnd, share);
    if (error == NO_ERROR) {
        RefreshAfterDriveChange(s);
        return;
    }
    ShowMapDriveError(s, error);  // app_network_page.cpp; nothing for a cancel
}

void RunDriveVerb(AppState& s, int cmd) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const std::vector<SelectedDrive> drives = SelectedDrives(*tab);
    // A verb on a multi-selection could eject/disconnect several volumes or
    // open several unlock prompts. Explorer's drive verbs are per-drive.
    if (drives.size() != 1) return;
    const SelectedDrive& drive = drives.front();
    if (cmd == app::CmdUnlockDrive && drive.locked) {
        app::StartBitLockerUnlock(s.hwnd, ServiceWindow(s), WM_BITLOCKER_UNLOCK, drive.root);
    } else if (cmd == app::CmdEjectDrive && app::DriveEjectable(drive.root, drive.type)) {
        app::StartDriveEject(s.hwnd, WM_DRIVE_VERB, drive.root, drive.type);
    } else if (cmd == app::CmdDisconnectDrive && drive.type == DRIVE_REMOTE) {
        app::StartDriveDisconnect(s.hwnd, WM_DRIVE_VERB, drive.root, false);
    }
}
} // namespace

bool UnpinSelectedNetworks(AppState& s) {
    bool changed = false;
    for (const std::wstring& pin : SelectedNetworkPins(s)) changed = s.places.UnpinNetwork(pin) || changed;
    if (!changed) return false;
    // The Network view lists the pins too (ShowNetworkPlaceMenu does the same).
    ForEachPane(s, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath)
            FillNetworkLocationsView(s, *tab);
    });
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void RenameSelected(AppState& s) {
    // A share, a network location, a Recycle Bin item or a locked drive has no
    // name to change here (drives rename their label).
    if (SelectionCapsOf(s).rename) ShowRenameOverlay(s);
}

void ApplyDriveVerbResult(AppState& s, const app::DriveVerbResult& result) {
    const std::wstring drive = DriveDisplay(result.root);
    if (result.error == NO_ERROR) {
        RefreshAfterDriveChange(s);
        return;
    }
    ui::ConfirmDialogSpec spec;
    spec.tone = ui::ConfirmTone::Warning;
    if (result.verb == app::DriveVerb::Disconnect && result.open_files) {
        spec.title = l10n::Get(l10n::StringId::Disconnect).c_str() + drive + l10n::Get(l10n::StringId::Text2738).c_str();
        spec.message = l10n::Get(l10n::StringId::FilesOrProgramsOnThisDriveAreOpenDisconnectingAnywayMayLoseU).c_str();
        spec.confirm_text = l10n::Get(l10n::StringId::DisconnectAnyway).c_str();
        spec.danger = true;
        spec.default_choice = ui::ConfirmChoice::Cancel;
        if (ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor))
            app::StartDriveDisconnect(s.hwnd, WM_DRIVE_VERB, result.root, true);
        return;
    }
    const bool eject = result.verb == app::DriveVerb::Eject;
    spec.title = (eject ? l10n::Get(l10n::StringId::CannotEject).c_str() :
        l10n::Get(l10n::StringId::CannotDisconnectNetworkDrive).c_str()) + drive;
    if (eject && result.error == ERROR_DEVICE_IN_USE) {
        spec.message = l10n::Get(l10n::StringId::TheDeviceIsInUseCloseAnyProgramsOrWindowsThatMightBeUsingFil).c_str();
        if (!result.veto_name.empty()) spec.note = l10n::Get(l10n::StringId::InUseBy).c_str() + result.veto_name;
    } else {
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, result.error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        spec.message = text ? text : L"";
        if (text) LocalFree(text);
        wchar_t code[64]{};
        swprintf_s(code, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), result.error);
        spec.note = code;
    }
    spec.confirm_text = l10n::Get(l10n::StringId::IntegrationRetry).c_str();
    spec.cancel_text = l10n::Get(l10n::StringId::Close).c_str();
    if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return;
    if (eject) {
        uint8_t type = static_cast<uint8_t>(GetDriveTypeW(result.root.c_str()));
        app::StartDriveEject(s.hwnd, WM_DRIVE_VERB, result.root, type);
    } else {
        app::StartDriveDisconnect(s.hwnd, WM_DRIVE_VERB, result.root, false);
    }
}

// 始终保留在此设备上 / 释放空间: flip the pin attributes of the selection (and
// everything under selected folders) off the UI thread, then report back with
// WM_CLOUD_PIN_DONE. The sync engine does the downloading or freeing; the
// watcher's attribute notifications redraw the rows as their state moves.
static void RunCloudPin(AppState& s, fs::CloudPinAction action) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || !app::IsUnderCloudFilesRoot(tab->current_path)) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    const HWND hwnd = s.hwnd;
    // Its own thread: a large pinned tree must not hold up folder loads queued
    // on the worker pool.
    std::thread([hwnd, action, paths = std::move(paths)] {
        const fs::CloudPinResult result = fs::ApplyCloudPin(paths, action);
        if (hwnd && IsWindow(hwnd))
            PostMessageW(hwnd, WM_CLOUD_PIN_DONE, static_cast<WPARAM>(action),
                         static_cast<LPARAM>(std::min<size_t>(result.failed, 0x7FFFFFFF)));
    }).detach();
}

static void FlipToolbarTool(AppState& s, size_t index);

void DispatchMenuCommand(AppState& s, int cmd) {
    if (HandleNetworkPageCommand(s, cmd)) return;  // 连接服务器, 最近访问, ...
    if (cmd >= app::CmdToolbarToolBase && cmd < app::CmdNewPrimaryBase &&
        cmd < app::CmdToolbarToolBase + static_cast<int>(app::kToolbarToolCount)) {
        const size_t tool = static_cast<size_t>(cmd - app::CmdToolbarToolBase);
        if (app::kToolbarToolIds[tool] == L"split") ShowSplitDropdown(s);
        else FlipToolbarTool(s, tool);
        return;
    }
    if (cmd == app::CmdManageAliases) {
        ShowAliasManager(s);
        return;
    }
    if (cmd >= app::CmdAliasDisplayBase && cmd < app::CmdAliasDisplayBase + 3) {
        app::AliasStore::Instance().SetDisplay(static_cast<app::AliasDisplay>(cmd - app::CmdAliasDisplayBase));
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (cmd >= app::CmdNewPrimaryBase && cmd < app::CmdNewPrimaryBase + 3) {
        s.appPrefs.toolbar_new_primary = cmd - app::CmdNewPrimaryBase;
        if (!s.isolatedTest) s.appPrefs.Save();
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    const bool needs_files=cmd==app::CmdProperties || cmd==app::CmdOpenPath || cmd==app::CmdOpenInNewTab || cmd==app::CmdRename ||
        cmd==app::CmdGroupIntoFolder || cmd==app::CmdDissolveFolder || cmd==app::CmdSetAlias;
    if(needs_files && DeferContentSelection(s,[cmd](AppState& v){DispatchMenuCommand(v,cmd);})) return;
    if (cmd >= app::CmdViewBase && cmd < app::CmdViewBase + 8) {
        ChooseViewMode(s, ui::ViewModeFromIndex(cmd - app::CmdViewBase));
        return;
    }
    if (cmd == app::CmdViewWall) {
        ShowIconWall(s, true);
        return;
    }
    switch (cmd) {
    case app::CmdCompressZip:
        CompressSelection(s);
        break;
    case app::CmdToggleFolderSizes:
        s.appPrefs.folder_size_auto = !s.appPrefs.folder_size_auto;
        if (!s.isolatedTest) s.appPrefs.Save();
        InvalidateRect(s.hwnd, nullptr, FALSE);
        break;
    case app::CmdApplyViewToAllFolders:
        ApplyViewToAllFolders(s);
        break;
    case app::CmdApplyGroupToAllFolders:
        if (const app::Tab* tab = ActiveTab(s)) ApplyGroupToAllFolders(s, tab->group_by);
        break;
    case app::CmdRefresh:
        RequestSidebarRefresh(s);
        s.renderer.RefreshFolderThumbnails();
        if (const auto* tab = ActiveTab(s)) {
            s.store.MarkDirty(tab->current_path);
            RefreshActiveTab(s);
        }
        break;
    case app::CmdFolderProperties:
        if (const auto* tab = ActiveTab(s); IsDeviceTab(tab)) {
            const std::wstring id = app::DeviceRegistry::Instance().FolderShellId(tab->current_path);
            if (!id.empty()) RunDeviceVerb(s, {id}, L"properties");
        } else if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
            s.ops.ShowProperties(ClipboardPath(tab->current_path));
        }
        break;
    case app::CmdSortName:
    case app::CmdSortModified:
    case app::CmdSortType:
    case app::CmdSortSize:
    case app::CmdSortPath:
        if (const auto* tab = ActiveTab(s)) {
            constexpr ui::SortColumn columns[] = { ui::SortColumn::Name, ui::SortColumn::Mtime,
                ui::SortColumn::Type, ui::SortColumn::Size, ui::SortColumn::Path };
            SetSort(s, columns[cmd - app::CmdSortName], tab->sort_direction);
        }
        break;
    case app::CmdSortCreated:
    case app::CmdSortAccessed:
        if (const auto* tab = ActiveTab(s))
            SetSort(s, cmd == app::CmdSortCreated ? ui::SortColumn::Created : ui::SortColumn::Accessed,
                    tab->sort_direction);
        break;
    case app::CmdSortAscending:
    case app::CmdSortDescending:
        if (const auto* tab = ActiveTab(s))
            SetSort(s, tab->sort_column, cmd == app::CmdSortAscending
                ? ui::SortDirection::Asc : ui::SortDirection::Desc);
        break;
    case app::CmdGroupNone:
    case app::CmdGroupName:
    case app::CmdGroupDate:
    case app::CmdGroupType:
    case app::CmdGroupSize:
    case app::CmdGroupTag:
    case app::CmdGroupLocation:
        SetGroupBy(s, cmd - app::CmdGroupNone);
        break;
    case app::CmdFolderSortTop:
    case app::CmdFolderSortFollow:
    case app::CmdFolderSortMixed:
        SetFolderSort(s, cmd - app::CmdFolderSortTop);
        break;
    case app::CmdOpen: OpenSelected(s); break;
    case app::CmdViewRecentChanges:
        if (const auto* tab = ActiveTab(s)) {
            const auto path = app::RecentChangesMenuPath(*tab, false);
            if (!path.empty()) OpenChangeView(s, path);
        }
        break;
    case app::CmdCompareSideBySide: OpenFoldersSideBySide(s); break;
    case app::CmdCompareToggle: {
        const app::LayoutTab* lt = s.window_tabs.Active();
        SetFolderCompare(s, !(lt && lt->compare));
        break;
    }
    case app::CmdCompareDiffOnly: ToggleCompareDiffOnly(s); break;
    case app::CmdOpenInNewTab: {
        app::Tab* tab = ActiveTab(s);
        if (!tab || !tab->snapshot) break;
        for (int index : tab->SelectedIndices()) {
            if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
            const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(index));
            if (!entry.is_dir) continue;
            const std::wstring path = EntryFullPath(*tab, index);
            if (!path.empty()) OpenFolderTab(s, path);
        }
        break;
    }
    case app::CmdOpenPath: {
        // Reveal the hit in its containing folder (search results).
        app::Tab* tab = ActiveTab(s);
        const std::wstring full = tab ? SelectedFullPath(s) : L"";
        if (!full.empty()) {
            const std::wstring parent = fs::ParentPath(full);
            const size_t slash = full.find_last_of(L"\\/");
            const std::wstring leaf =
                slash == std::wstring::npos ? full : full.substr(slash + 1);
            if (!parent.empty() && !leaf.empty()) {
                NavigateTo(s, parent);
                tab->pending_selected_name = leaf;
                tab->pending_selected_names = { leaf };
                tab->pending_selection_revision = tab->selection_revision;
                tab->pending_ensure_selection_visible = true;
                if (tab->snapshot) {
                    for (size_t i = 0; i < tab->snapshot->size(); ++i) {
                        if (_wcsicmp((*tab->snapshot)[i].name.c_str(), leaf.c_str()) != 0) continue;
                        tab->SelectOnly(static_cast<int>(i));
                        tab->pending_selection_revision = tab->selection_revision;
                        EnsureRowVisible(s, *tab, static_cast<int>(i));
                        break;
                    }
                }
            }
        }
        break;
    }
    case app::CmdCut: CollectToTray(s, true); break;
    case app::CmdCopy: CollectToTray(s, false); break;
    case app::CmdPaste: PasteIntoCurrent(s); break;
    case app::CmdDelete: DeleteSelected(s, (GetKeyState(VK_SHIFT) & 0x8000) != 0); break;
    case app::CmdRename: RenameSelected(s); break;
    case app::CmdEjectDrive:
    case app::CmdDisconnectDrive:
    case app::CmdUnlockDrive: RunDriveVerb(s, cmd); break;
    case app::CmdMapNetworkDrive: RunMapNetworkDrive(s, SelectedShare(s)); break;
    case app::CmdUnpinNetwork: UnpinSelectedNetworks(s); break;
    case app::CmdBatchRename: ShowBatchRename(s); break;
    case app::CmdGroupIntoFolder: GroupSelectionIntoFolder(s); break;
    case app::CmdDissolveFolder: DissolveSelectedFolders(s); break;
    case app::CmdSetAlias: ShowAliasOverlay(s); break;
    case app::CmdStageToTray: StageSelection(s); break;
    case app::CmdCloudKeep: RunCloudPin(s, fs::CloudPinAction::Keep); break;
    case app::CmdCloudFree: RunCloudPin(s, fs::CloudPinAction::Free); break;
    case app::CmdAdvancedSearch: ShowAdvancedSearch(s); break;
    case app::CmdRestoreRecycle: RestoreSelected(s); break;
    case app::CmdRestoreAllRecycle: RestoreAllRecycle(s); break;
    case app::CmdEmptyRecycle: EmptyRecycleBin(s); break;
    case app::CmdOpenRecycle: NavigateTo(s, app::MakeRecyclePath()); break;
    case app::CmdSelectAll: {
        app::Tab* tab = ActiveTab(s);
        if (!tab) break;
        std::vector<int> matches;
        app::CollectFilterMatches(*tab, &s.places, matches);
        tab->SelectIndices(matches);
        if (tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
        break;
    }
    case app::CmdInvertSelection: {
        app::Tab* tab = ActiveTab(s);
        if (!tab) break;
        std::vector<int> matches;
        app::CollectFilterMatches(*tab, &s.places, matches);
        tab->InvertIndices(matches);
        if (tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
        break;
    }
    case app::CmdSelectWildcard:
        ShowWildcardSelect(s);
        break;
    case app::CmdProperties: {
        // Shell properties on the ops open thread. A multi-selection opens one
        // merged sheet (like Explorer) instead of the focused item's sheet.
        std::vector<std::wstring> paths;
        if (const app::Tab* pc_tab = ActiveTab(s)) {
            if (std::wstring id = PcDeviceShellId(*pc_tab, pc_tab->SelectedIndices()); !id.empty()) {
                RunDeviceVerb(s, {std::move(id)}, L"properties");  // the phone itself
                break;
            }
        }
        if (const app::Tab* device_tab = ActiveTab(s); IsDeviceTab(device_tab)) {
            // A phone item's own property sheet, from the shell host.
            RunDeviceVerb(s, SelectedShellIds(*device_tab), L"properties");
            break;
        }
        if (const app::Tab* tab = ActiveTab(s); IsRecycleTab(tab) && tab->snapshot) {
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
                const fs::DirEntry entry = tab->EntryAt(static_cast<size_t>(index));
                const std::wstring& full = entry.recycle_path.empty() ? entry.full_path : entry.recycle_path;
                if (!full.empty()) paths.push_back(ClipboardPath(full));
            }
        } else if (tab) {
            // Focused item first so single-sheet fallbacks show what the user right-clicked.
            const std::wstring focused = SelectedFullPath(s);
            if (!focused.empty()) paths.push_back(ClipboardPath(focused));
            for (const auto& full : SelectedFullPaths(*tab)) {
                if (full != focused) paths.push_back(ClipboardPath(full));
            }
        }
        if (!paths.empty()) s.ops.ShowProperties(paths);
        break;
    }
    case app::CmdOpenTerminal: {
        const std::wstring target = TerminalTarget(s);
        if (!target.empty()) s.ops.OpenTerminal(ClipboardPath(target));
        break;
    }
    case app::CmdCopyPath: {
        // Item menu copies the selection; background menu copies the folder.
        app::Tab* tab = ActiveTab(s);
        if (tab && tab->SelectedCount() > 0) {
            CopySelectedPath(s);
            break;
        }
        std::wstring full = tab ? tab->current_path : L"";
        if (!full.empty())
            ops::WriteClipboardText(fs::IsDevicePath(full) ? DeviceFriendlyPath(full) : ClipboardPath(full));
        break;
    }
    case app::CmdUndo:
        if (s.ops.CanUndo()) s.ops.Undo();
        break;
    case app::CmdNewFolder: CreateNewItem(s, true); break;
    case app::CmdNewTextFile: CreateNewItem(s, false); break;
    case app::CmdNewMarkdownFile: CreateNewMarkdown(s); break;
    case app::CmdTags: {
        POINT point{};
        GetCursorPos(&point);
        ShowTagPicker(s, point);
        break;
    }
    case app::CmdCustomizeToolbar:
        ShowToolbarCustomize(s);
        break;
    case app::CmdShortcutHelp:
        ui::ShowShortcutHelp(s.hwnd, s.darkMode, s.accentColor);
        break;
    case app::CmdColumnLayout:
        ToggleColumnLayout(s,-1);
        break;
    case app::CmdDetailsPanel:
        s.showDetailsPanel = !s.showDetailsPanel;
        s.renderer.SetDetailsPanelVisible(s.showDetailsPanel, true);
        break;
    case app::CmdLayoutSingle: ApplyLayoutPreset(s, app::LayoutPreset::Single); break;
    case app::CmdLayoutTwoVertical: ApplyLayoutPreset(s, app::LayoutPreset::TwoVertical); break;
    case app::CmdLayoutTwoHorizontal: ApplyLayoutPreset(s, app::LayoutPreset::TwoHorizontal); break;
    case app::CmdLayoutThree: ApplyLayoutPreset(s, app::LayoutPreset::Three); break;
    case app::CmdLayoutFourGrid: ApplyLayoutPreset(s, app::LayoutPreset::FourGrid); break;
    case app::CmdCopyToTarget: TransferToTarget(s, false); break;
    case app::CmdMoveToTarget: TransferToTarget(s, true); break;
    case app::CmdPinWorkspace: {
        std::wstring root = PinCandidate(s);
        if (!root.empty() && !fs::IsVirtualPath(root)) {
            if (s.places.FindWorkspace(root) >= 0) {
                s.places.UnpinWorkspace(root);
            } else {
                s.places.PinWorkspace(root, L"", static_cast<int>(WorkspaceLayoutOf(s)), CollectPanePaths(s),
                                      CollectPaneViews(s));
            }
        }
        break;
    }
    case app::CmdPinNetwork: {
        std::wstring root = PinCandidate(s);
        if (root.empty()) {
            app::Tab* tab = ActiveTab(s);
            if (tab) root = tab->current_path;
        }
        if (IsUncPath(root)) {
            s.places.PinNetwork(root, L"");
            RequestUncProbe(s, root);
            RefreshNetworkPages(s);  // a 最近访问 / 局域网设备 row moves up to 已固定
        }
        break;
    }
    case app::CmdSearchAll: {
        const auto q = app::ParseOmnibarQuery(s.paletteQuery, false);
        if (q.needle.empty()) break;
        NavigateTo(s, app::MakeSearchPath(q.needle));
        break;
    }
    case app::CmdInstallFullIndex:
        s.index.RequestInstallService();
        break;
    case app::CmdSettings:
        OpenSettingsTab(s, 0);
        break;
    case app::CmdExitPulse:
        PostMessageW(s.hwnd, WM_EXIT_PULSE, 0, 0);
        break;
    case app::CmdSettingsContextMenu:
        OpenSettingsTab(s, 2);
        break;
    default:
            if (cmd >= app::CmdIndexBase) {
                const int idx = cmd - app::CmdIndexBase;
                if (idx >= 0 && idx < static_cast<int>(s.paletteHits.size())) {
                    const auto& hit = s.paletteHits[static_cast<size_t>(idx)];
                    if (hit.is_dir) NavigateTo(s, hit.path);
                    else {
                        s.ops.OpenWith(hit.path);
                        RecordRecentOpen(s, hit.path, app::PlaceItemKind::File);
                    }
                }
        } else if (cmd >= app::CmdRecentBase) {
            const int idx = cmd - app::CmdRecentBase;
            const auto recent = s.places.RecentFolderPaths();
            if (idx >= 0 && idx < static_cast<int>(recent.size()))
                NavigateTo(s, recent[static_cast<size_t>(idx)]);
        }
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ClipboardHasFiles() {
    return ops::ClipboardHasPasteableFiles();
}

void ApplyWorkspacePinLabel(std::vector<ui::FluentMenuItem>& items, AppState& s) {
    const std::wstring root = PinCandidate(s);
    const bool pinned = !root.empty() && !fs::IsVirtualPath(root) &&
        s.places.FindWorkspace(root) >= 0;
    for (auto& item : items) {
        if (item.command != app::CmdPinWorkspace) continue;
        item.text = pinned ? l10n::Get(l10n::StringId::UnpinWorkspace)
                           : l10n::Get(l10n::StringId::PinWorkspace);
        break;
    }
}

namespace {
ui::FluentMenuItem VerbRow(int command, std::wstring text, const wchar_t* glyph) {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = std::move(text);
    item.glyph = glyph;
    item.enabled = true;
    return item;
}

void EraseCommand(std::vector<ui::FluentMenuItem>& items, int command) {
    std::erase_if(items, [command](const ui::FluentMenuItem& item) { return item.command == command; });
}

bool HasCommand(const std::vector<ui::FluentMenuItem>& items, int command) {
    return std::any_of(items.begin(), items.end(), [command](const ui::FluentMenuItem& item) { return item.command == command; });
}

// 固定到网络 pins what CmdPinNetwork would: a share or a folder on one that is
// not pinned yet. Elsewhere (local folders, phones, Pulse views) it did
// nothing, so it is not offered.
bool CanPinNetwork(AppState& s) {
    const std::wstring root = PinCandidate(s);
    return fs::IsUncPath(root) && s.places.FindNetwork(root) < 0;
}

// Drives, shares and network locations get their own verbs (弹出, 断开连接,
// 解锁, 映射网络驱动器, 取消固定) instead of the file ones, as in Explorer.
void AdaptItemMenu(AppState& s, std::vector<ui::FluentMenuItem>& items, const app::SelectionCaps& caps) {
    if (TerminalTarget(s).empty()) EraseCommand(items, app::CmdOpenTerminal);
    if (!CanPinNetwork(s)) EraseCommand(items, app::CmdPinNetwork);
    if (caps.kind == app::SelectionKind::Files || caps.kind == app::SelectionKind::None) return;
    if (caps.kind == app::SelectionKind::Launchers) {
        // Program icons in This PC: opening the program is all they do. A
        // phone keeps 属性; its other rows are the shell's, from the host.
        const app::Tab* pc_tab = ActiveTab(s);
        const bool phone = pc_tab && !PcDeviceShellId(*pc_tab, pc_tab->SelectedIndices()).empty();
        std::erase_if(items, [phone](const ui::FluentMenuItem& item) {
            return item.command != app::CmdOpen && !(phone && item.command == app::CmdProperties);
        });
        for (auto& item : items) item.separator_after = false;
        return;
    }
    // The 剪切 / 复制 / 删除 / 重命名 strip keeps what this selection allows.
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->command != app::CmdNone || it->quick_swatches.empty()) continue;
        std::erase_if(it->quick_swatches, [&](const ui::FluentMenuSwatch& button) {
            return (button.command == app::CmdCut && !caps.cut) || (button.command == app::CmdCopy && !caps.copy) ||
                (button.command == app::CmdDelete && !caps.del) || (button.command == app::CmdRename && !caps.rename);
        });
        if (it->quick_swatches.empty()) {
            if (it != items.begin()) std::prev(it)->separator_after = true;
            items.erase(it);
        }
        break;
    }
    EraseCommand(items, app::CmdTags);  // the swatch strip and 标签...
    EraseCommand(items, app::CmdBatchRename);
    EraseCommand(items, app::CmdGroupIntoFolder);
    EraseCommand(items, app::CmdDissolveFolder);
    if (caps.kind == app::SelectionKind::Device) {
        // A phone item: what Explorer offers for it. Pulse's own path-based
        // features (pins, aliases, terminal, history, zip) need a real path.
        for (int command : {app::CmdOpenTerminal, app::CmdPinWorkspace, app::CmdPinQuickAccess,
                            app::CmdUnpinQuickAccess, app::CmdPinNetwork, app::CmdViewRecentChanges,
                            app::CmdCompareSideBySide, app::CmdSetAlias, app::CmdStageToTray,
                            app::CmdCompressZip, app::CmdOpenPath, app::CmdCloudKeep, app::CmdCloudFree})
            EraseCommand(items, command);
        return;
    }
    std::vector<ui::FluentMenuItem> verbs;
    const app::Tab* tab = ActiveTab(s);
    if (caps.kind == app::SelectionKind::Drives && tab) {
        EraseCommand(items, app::CmdPinNetwork);
        if (caps.all_locked) {
            for (int command : {app::CmdOpenInNewTab, app::CmdPinWorkspace, app::CmdViewRecentChanges,
                                app::CmdPinQuickAccess, app::CmdUnpinQuickAccess})
                EraseCommand(items, command);
        }
        const std::vector<SelectedDrive> drives = SelectedDrives(*tab);
        if (drives.size() == 1) {
            const SelectedDrive& drive = drives.front();
            if (drive.locked)
                verbs.push_back(VerbRow(app::CmdUnlockDrive, l10n::Get(l10n::StringId::UnlockDrive).c_str(), L"\xE785"));
            if (app::DriveEjectable(drive.root, drive.type))
                verbs.push_back(VerbRow(app::CmdEjectDrive, l10n::Get(l10n::StringId::Eject).c_str(), app::EjectGlyph()));
            if (drive.type == DRIVE_REMOTE)
                verbs.push_back(VerbRow(app::CmdDisconnectDrive,
                                        l10n::Get(l10n::StringId::DisconnectNetworkDrive).c_str(), L"\xE8CD"));
        }
    } else if (caps.kind == app::SelectionKind::NetworkShares) {
        const std::wstring share = SelectedShare(s);
        if (!share.empty())
            verbs.push_back(VerbRow(app::CmdMapNetworkDrive, l10n::Get(l10n::StringId::MapNetworkDrive).c_str(), L"\xE8CE"));
        // 钉为网络位置 stays reachable under the 标准 menu preset here.
        if (!share.empty() && s.places.FindNetwork(share) < 0 && !HasCommand(items, app::CmdPinNetwork))
            verbs.push_back(VerbRow(app::CmdPinNetwork, l10n::Get(l10n::StringId::PinNetwork).c_str(),
                                    ui::command_icons::kGlyphPinNetwork));
    } else if (caps.kind == app::SelectionKind::NetworkPlaces) {
        // NetHood shortcuts are local .lnk files: don't offer actions that would
        // accidentally pin the shortcut file as a workspace or Quick Access.
        EraseCommand(items, app::CmdPinWorkspace);
        EraseCommand(items, app::CmdPinQuickAccess);
        EraseCommand(items, app::CmdUnpinQuickAccess);
        if (caps.any_network_pin) {
            verbs.push_back(VerbRow(app::CmdUnpinNetwork, l10n::Get(l10n::StringId::Unpin), L"\xE77A"));
            // 重命名: Pulse's name for one pin (the share itself is not renamed).
            if (!SelectedNetworkPin(s).empty()) {
                EraseCommand(items, app::CmdRename);
                verbs.push_back(VerbRow(app::CmdRenameNetwork, l10n::Get(l10n::StringId::Rename), L"\xE8AC"));
            }
            EraseCommand(items, app::CmdPinNetwork);
        } else {
            const auto paths = tab ? SelectedFullPaths(*tab) : std::vector<std::wstring>{};
            if (std::none_of(paths.begin(), paths.end(), [](const std::wstring& path) { return fs::IsUncPath(path); }))
                EraseCommand(items, app::CmdPinNetwork);
            else if (!HasCommand(items, app::CmdPinNetwork))  // 最近访问 / 局域网设备 rows, any menu preset
                verbs.push_back(VerbRow(app::CmdPinNetwork, l10n::Get(l10n::StringId::PinNetwork).c_str(),
                                        ui::command_icons::kGlyphPinNetwork));
        }
        if (!SelectedShare(s).empty())
            verbs.push_back(VerbRow(app::CmdMapNetworkDrive, l10n::Get(l10n::StringId::MapNetworkDrive).c_str(), L"\xE8CE"));
        if (tab) {
            // 网络驱动器 rows: 断开连接, as on This PC.
            const std::vector<SelectedDrive> drives = SelectedDrives(*tab);
            if (drives.size() == 1 && drives.front().type == DRIVE_REMOTE)
                verbs.push_back(VerbRow(app::CmdDisconnectDrive,
                                        l10n::Get(l10n::StringId::DisconnectNetworkDrive).c_str(), L"\xE8CD"));
            if (SelectionHasNetworkRecent(*tab))
                verbs.push_back(VerbRow(app::CmdForgetNetworkRecent,
                                        l10n::Get(l10n::StringId::NetworkForgetRecent).c_str(), L"\xE711"));
        }
    }
    if (verbs.empty()) return;
    verbs.back().separator_after = true;
    // Right after 打开.
    auto open = std::find_if(items.begin(), items.end(),
                             [](const ui::FluentMenuItem& item) { return item.command == app::CmdOpen; });
    items.insert(open == items.end() ? items.begin() : open + 1, verbs.begin(), verbs.end());
}

// A selection inside a Cloud Files folder (OneDrive, WPS网盘, ...) gets
// 始终保留在此设备上 / 释放空间 right after 打开, as Explorer does. Each greys
// out when it would change nothing for any selected item.
void AppendCloudPinCommands(const app::Tab& tab, std::vector<ui::FluentMenuItem>& items) {
    if (!tab.snapshot || tab.SelectedCount() == 0 || !app::IsUnderCloudFilesRoot(tab.current_path)) return;
    bool can_keep = false, can_free = false, all_remote_files = true;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) continue;
        const fs::DirEntry entry = tab.EntryAt(static_cast<size_t>(index));
        const fs::CloudState state = fs::CloudStateFromAttrs(entry.attrs);
        can_keep |= state != fs::CloudState::Pinned;
        can_free |= state != fs::CloudState::Online;
        all_remote_files &= !entry.is_dir && fs::IsCloudPlaceholder(entry.attrs);
    }
    ui::FluentMenuItem keep = VerbRow(app::CmdCloudKeep, l10n::Get(l10n::StringId::CloudKeepOnDevice), L"\xE718");
    keep.enabled = can_keep;
    ui::FluentMenuItem release = VerbRow(app::CmdCloudFree, l10n::Get(l10n::StringId::CloudFreeUpSpace), L"\xE753");
    release.enabled = can_free;
    release.separator_after = true;
    auto open = std::find_if(items.begin(), items.end(),
                             [](const ui::FluentMenuItem& item) { return item.command == app::CmdOpen; });
    if (open != items.end()) {
        open->separator_after = true;
        // 打开 is the user asking for the content: the sync engine downloads it
        // first, so say so (design: 打开 · 会先下载).
        if (all_remote_files && open->shortcut.empty())
            open->shortcut = l10n::Get(l10n::StringId::CloudOpenDownloadsFirst);
    }
    const ui::FluentMenuItem rows[] = {std::move(keep), std::move(release)};
    items.insert(open == items.end() ? items.begin() : open + 1, std::begin(rows), std::end(rows));
}
} // namespace

std::vector<ui::FluentMenuItem> BuildFinderItemMenu(
        AppState& s, bool can_undo, const std::wstring& undo_label) {
    if (IsRecycleTab(ActiveTab(s)))
        return app::BuildRecycleItemMenu(can_undo, undo_label);
    bool folder = false;
    if (const app::Tab* tab = ActiveTab(s); tab && tab->snapshot) {
        for (int index : tab->SelectedIndices()) {
            if (index >= 0 && index < static_cast<int>(tab->EntryCount()) &&
                tab->EntryAt(static_cast<size_t>(index)).is_dir) {
                folder = true;
                break;
            }
        }
    }
    std::vector<ui::FluentMenuItem> items = app::BuildItemMenu(can_undo, undo_label, folder);
    if (const auto* tab = ActiveTab(s))
        app::AppendRecentChangesCommand(items, app::RecentChangesMenuPath(*tab, false));
    ApplyWorkspacePinLabel(items, s);
    AppendQuickAccessCommand(s, items, QuickAccessTargets(ActiveTab(s), false));
    const std::vector<std::wstring> paths = ActiveTab(s)
        ? SelectedFullPaths(*ActiveTab(s)) : std::vector<std::wstring>{};
    // The color row shows the favorite tags, in slot order (Alt+1..9).
    ui::FluentMenuItem quick_tags;
    quick_tags.command = app::CmdTags;
    for (int slot = 1; slot <= app::kMaxFavoriteTags; ++slot) {
        const int index = s.places.FavoriteTagIndex(slot);
        if (index < 0) continue;
        const auto& tag = s.places.tags[static_cast<size_t>(index)];
        ui::FluentMenuSwatch swatch;
        swatch.command = app::CmdTagBase + slot - 1;
        swatch.color = ui::HexColor(tag.rgb);
        const auto state = s.places.GetSelectionState(tag.id, paths);
        swatch.checked = state == app::TagSelectionState::All;
        swatch.mixed = state == app::TagSelectionState::Mixed;
        quick_tags.quick_swatches.push_back(std::move(swatch));
    }
    const auto picker = std::find_if(items.begin(), items.end(), [](const ui::FluentMenuItem& item) {
        return item.command == app::CmdTags;
    });
    if (!quick_tags.quick_swatches.empty()) items.insert(picker, std::move(quick_tags));
    // Search results mix folders, so offer "reveal in containing folder"
    // right after 打开 (item menus only; pointless in a real folder view).
    const app::Tab* tab = ActiveTab(s);
    std::wstring view_kind;
    if (tab && app::ParsePulsePath(tab->current_path, &view_kind, nullptr) &&
        view_kind == L"search") {
        ui::FluentMenuItem open_path;
        open_path.command = app::CmdOpenPath;
        open_path.text = l10n::Get(l10n::StringId::OpenPath);
        open_path.glyph = ui::command_icons::kGlyphOpenPath;
        items.insert(items.begin() + 1, std::move(open_path));
    }
    if (CanDissolveSelection(tab)) {
        ui::FluentMenuItem dissolve;
        dissolve.command = app::CmdDissolveFolder;
        dissolve.text = l10n::Get(l10n::StringId::UngroupFolder_2).c_str();
        dissolve.glyph = L"\xED25"; // open folder
        dissolve.shortcut = app::ShownShortcut(L"Ctrl+Shift+G");
        items.insert(items.begin() + 1, std::move(dissolve));
    }
    if (CanGroupSelection(tab)) {
        ui::FluentMenuItem group;
        group.command = app::CmdGroupIntoFolder;
        group.text = l10n::Get(l10n::StringId::NewFolderWithSelection_2).c_str();
        group.glyph = ui::command_icons::kGlyphGroupFolder;
        group.shortcut = app::ShownShortcut(L"Ctrl+G");
        items.insert(items.begin() + 1, std::move(group));
    }
    // 别名: one real item can take a display name (after 复制路径).
    if (tab && tab->SelectedCount() == 1 && !fs::IsVirtualPath(tab->current_path) && !IsRecycleTab(tab)) {
        ui::FluentMenuItem alias;
        alias.command = app::CmdSetAlias;
        alias.text = l10n::Get(l10n::StringId::SetAliasEllipsis);
        alias.glyph = L"\xE8EC";  // tag
        alias.shortcut = app::ShownShortcut(L"Shift+F2");
        auto at = std::find_if(items.begin(), items.end(),
            [](const ui::FluentMenuItem& item) { return item.command == app::CmdCopyPath; });
        items.insert(at == items.end() ? items.end() : std::next(at), std::move(alias));
    }
    // 加入暂存区: stage without the clipboard (after 复制路径 / 设置别名).
    if (tab && tab->SelectedCount() > 0 && !IsRecycleTab(tab) && SelectionCapsOf(s).copy) {
        ui::FluentMenuItem stage;
        stage.command = app::CmdStageToTray;
        stage.text = l10n::Get(l10n::StringId::CtxAddToTray);
        stage.glyph = L"\xE7B8";  // package
        stage.shortcut = app::ShownShortcut(L"Ctrl+Shift+S");
        auto at = std::find_if(items.begin(), items.end(), [](const ui::FluentMenuItem& item) {
            return item.command == app::CmdSetAlias || item.command == app::CmdCopyPath;
        });
        if (at != items.end() && at->command == app::CmdCopyPath) {
            auto alias = std::next(at);
            if (alias != items.end() && alias->command == app::CmdSetAlias) at = alias;
        }
        items.insert(at == items.end() ? items.end() : std::next(at), std::move(stage));
    }
    // 复制到 / 移动到目标栏 (#58-14, moved here from the layout menu): split
    // only, right-clicked in a pane that is not the target; greyed when the
    // target cannot take files, as TransferToTarget would refuse.
    if (tab && tab->SelectedCount() > 0 && !IsRecycleTab(tab) && s.targetPane && s.targetPane != s.pane &&
        LiveLayout(s).VisiblePanes().size() > 1) {
        const app::SelectionCaps caps = SelectionCapsOf(s);
        const app::Tab* dst = s.targetPane->ActiveTab();
        const bool writable = dst && !dst->current_path.empty() && !fs::IsVirtualPath(dst->current_path) &&
            !dst->net_readonly && !app::IsUncServerRoot(dst->current_path);
        if (caps.copy || caps.cut) {
            const struct { int command; l10n::StringId label; const wchar_t* glyph; const wchar_t* key; bool enabled; } kRows[] = {
                {app::CmdCopyToTarget, l10n::StringId::CopyToTarget, ui::command_icons::kGlyphCopyToPane, L"Ctrl+Alt+C",
                 writable && caps.copy},
                {app::CmdMoveToTarget, l10n::StringId::MoveToTarget, ui::command_icons::kGlyphMoveToPane, L"Ctrl+Alt+X",
                 writable && caps.cut},
            };
            // After 加入暂存区 / 设置别名 / 复制路径, whichever is last present.
            auto at = items.end();
            for (int command : {app::CmdCopyPath, app::CmdSetAlias, app::CmdStageToTray}) {
                auto found = std::find_if(items.begin(), items.end(),
                    [command](const ui::FluentMenuItem& item) { return item.command == command; });
                if (found != items.end()) at = std::next(found);
            }
            for (const auto& row : kRows) {
                ui::FluentMenuItem item;
                item.command = row.command;
                item.text = l10n::Get(row.label);
                item.glyph = row.glyph;
                item.shortcut = app::ShownShortcut(row.key);
                item.enabled = row.enabled;
                at = std::next(items.insert(at, std::move(item)));
            }
        }
    }
    if (tab && tab->SelectedCount() >= 2) {
        ui::FluentMenuItem batch;
        batch.command = app::CmdBatchRename;
        batch.text = l10n::Get(l10n::StringId::BatchRename);
        batch.glyph = ui::command_icons::kGlyphBatchRename;
        batch.shortcut = app::ShownShortcut(L"Ctrl+Shift+R");
        items.insert(items.begin() + 1, std::move(batch));
    }
    if (tab && SideBySideFolders(*tab).size() == 2) {
        ui::FluentMenuItem compare;
        compare.command = app::CmdCompareSideBySide;
        compare.text = l10n::Get(l10n::StringId::CompareSideBySide);
        compare.glyph = ui::command_icons::kGlyphCompare;
        items.insert(items.begin() + 1, std::move(compare));
    }
    if (tab) AdaptItemMenu(s, items, app::ComputeSelectionCaps(*tab, &s.places));
    if (tab) AppendCloudPinCommands(*tab, items);
    return items;
}

// Lowercased common extension of the selection; "" for folders / mixed types.
std::wstring CommonExtension(const app::Tab& tab,
                                    const std::vector<int>& indices) {
    if (!tab.snapshot) return L"";
    std::wstring ext;
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) return L"";
        const fs::DirEntry& e = tab.EntryAt(static_cast<size_t>(index));
        if (e.is_dir) return L"";
        const auto pos = e.name.find_last_of(L'.');
        if (pos == std::wstring::npos || pos == 0 || pos + 1 >= e.name.size()) return L"";
        std::wstring one = e.name.substr(pos);
        for (auto& c : one) c = static_cast<wchar_t>(std::towlower(c));
        if (ext.empty()) ext = std::move(one);
        else if (ext != one) return L"";
    }
    return ext;
}

bool IsDriveRootPath(std::wstring path) {
    path = pulse::path::StripExtendedPathPrefix(path);
    if (path.size() >= 2 && path[1] == L':') {
        if (path.size() == 2) return true;
        if (path.size() == 3 && (path[2] == L'\\' || path[2] == L'/')) return true;
    }
    return false;
}

std::wstring StaticVerbKey(const app::Tab& tab, const std::vector<int>& indices) {
    const std::wstring ext = CommonExtension(tab, indices);
    if (!ext.empty()) return ext;
    if (!tab.snapshot || indices.empty()) return L"";
    bool all_dirs = true;
    bool all_drives = true;
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) return L"";
        const fs::DirEntry& e = tab.EntryAt(static_cast<size_t>(index));
        if (!e.is_dir) {
            all_dirs = false;
            all_drives = false;
            break;
        }
        if (!IsDriveRootPath(EntryFullPath(tab, index))) all_drives = false;
    }
    if (!all_dirs) return L"";
    return all_drives ? std::wstring(ipc::kDriveVerbKey) : std::wstring(ipc::kFolderVerbKey);
}

// Registry static verbs are read off the UI thread and cached per extension;
// the reader posts WM_SHELL_VERBS back when done.
void PrefetchStaticVerbs(AppState& s, const std::wstring& ext) {
    // A cached type is read again now and then: verbs under its own ProgID are
    // outside the registry watch (ShellMenuWatchKeys).
    if (!s.context_menu.RequestStaticPrefetch(ext) &&
        !s.context_menu.RequestStaticRevalidate(ext, GetTickCount64()))
        return;
    HWND hwnd = ServiceWindow(s);
    const uint32_t cache_generation = s.context_menu.cache_generation();
    std::thread([hwnd, ext, cache_generation] {
        auto verbs = app::EnumerateStaticVerbs(ext);
        app::ResolveStaticVerbIcons(verbs);
        auto* result = new ShellVerbsResult{ ext, std::move(verbs), cache_generation };
        if (!PostMessageW(hwnd, WM_SHELL_VERBS, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

void StartShellRegistryWatch(HWND hwnd) {
    if (g_shell_watcher.running()) return;
    g_shell_watcher.Start(app::ShellMenuWatchKeys(), [hwnd] {
        PostMessageW(hwnd, WM_SHELL_CACHE_INVALIDATE, 0, 0);
    });
}

void StopShellRegistryWatch() { g_shell_watcher.Stop(); }

void SeedShellVerbCache(AppState& s) {
    HWND hwnd = ServiceWindow(s);
    // A newer seed supersedes this one; stop instead of piling up readers.
    const uint32_t generation = ++g_shell_seed_generation;
    const uint32_t cache_generation = s.context_menu.cache_generation();
    std::thread([hwnd, generation, cache_generation] {
        // Re-reading up to 400 types costs about 2 s of CPU right at startup
        // (and after every Classes change), when the first screen of
        // thumbnails is decoding: background CPU and I/O priority lets those
        // go first. The thread is detached and ends here, so no END call.
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        // The cache file can be megabytes: read and parse it here, never on
        // the UI thread, which only merges the result (WM_SHELL_VERB_SEED).
        auto seed = std::make_unique<ShellVerbSeed>();
        seed->generation = generation;
        if (!app::LoadMachineStaticVerbCache(seed->machine) || seed->machine.empty()) return;
        std::vector<std::wstring> extensions;
        extensions.reserve(seed->machine.size());
        for (const auto& [ext, verbs] : seed->machine) extensions.push_back(ext);
        if (g_shell_seed_generation.load() != generation) return;
        if (!PostMessageW(hwnd, WM_SHELL_VERB_SEED, 0, reinterpret_cast<LPARAM>(seed.get())))
            return;
        seed.release();
        // Posted after the seed, so the merge always lands first.
        size_t n = 0;
        for (const auto& ext : extensions) {
            // The window goes away on exit: stop rather than keep resolving
            // icons while the process tears down.
            if (n++ > 400 || g_shell_seed_generation.load() != generation || !IsWindow(hwnd))
                break;
            auto verbs = app::EnumerateStaticVerbs(ext);
            app::ResolveStaticVerbIcons(verbs);
            auto* result = new ShellVerbsResult{ ext, std::move(verbs), cache_generation };
            if (!PostMessageW(hwnd, WM_SHELL_VERBS, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
        }
    }).detach();
}

void ApplyShellVerbSeed(AppState& s, ShellVerbSeed& seed) {
    // Caches were invalidated after this seed was read; its successor follows.
    if (seed.generation != g_shell_seed_generation.load()) return;
    s.context_menu.MergeStaticCache(std::move(seed.machine));
}

namespace {

std::wstring CtxLayoutBuild() {
    return std::wstring(PULSE_VERSION_STRING) + L"+" + PULSE_BUILD_ID;
}

// Writes ctx_layout.bin off the UI thread, 1.5 s after the last change, so a
// burst of right-clicks costs one write. Discard() drops a pending snapshot
// and abandons a write in progress: the registry changed and its rows may be
// stale (they are read again from the file, where fingerprints sort them out).
class CtxLayoutSaver {
public:
    ~CtxLayoutSaver() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    void Schedule(app::CtxLayoutMap layout) {
        {
            std::lock_guard lock(mutex_);
            pending_ = std::move(layout);
            has_pending_ = true;
            due_ = GetTickCount64() + kDelayMs;
            if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
        }
        wake_.notify_one();
    }
    void Discard() {
        std::lock_guard lock(mutex_);
        has_pending_ = false;
        pending_.clear();
        ++epoch_;
    }
private:
    static constexpr uint64_t kDelayMs = 1500;
    void Run() noexcept {
        try {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (!has_pending_ && !stopping_) {
                    wake_.wait(lock);
                    continue;
                }
                if (!has_pending_) return;
                const uint64_t now = GetTickCount64();
                // At exit the last snapshot is written at once.
                if (!stopping_ && now < due_) {
                    wake_.wait_for(lock, std::chrono::milliseconds(due_ - now));
                    continue;
                }
                app::CtxLayoutMap layout = std::move(pending_);
                pending_.clear();
                has_pending_ = false;
                const uint64_t epoch = epoch_;
                lock.unlock();
                app::SaveCtxLayoutFile(app::CtxLayoutCachePath(), CtxLayoutBuild(), layout,
                    app::CtxLayoutFingerprint, [this, epoch] {
                        std::lock_guard guard(mutex_);
                        return epoch == epoch_;
                    });
                lock.lock();
            }
        } catch (...) {
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    app::CtxLayoutMap pending_;
    bool has_pending_ = false;
    bool stopping_ = false;
    uint64_t due_ = 0;
    uint64_t epoch_ = 0;
};

CtxLayoutSaver g_ctx_layout_saver;

} // namespace

// A test instance keeps out of the user's profile: it reads and writes the
// layout only when its persistence is redirected (selftest builds) or
// PULSE_CTX_LAYOUT_PATH names a file.
static bool CtxLayoutPersists(const AppState& s) {
    if (!s.isolatedTest || s.isolatedTestPersist) return true;
    return GetEnvironmentVariableW(L"PULSE_CTX_LAYOUT_PATH", nullptr, 0) > 0;
}

void SeedCtxLayout(AppState& s) {
    // Without a loaded layout nothing is ever saved either.
    if (!CtxLayoutPersists(s)) return;
    HWND hwnd = ServiceWindow(s);
    const uint32_t generation = s.context_menu.cache_generation();
    std::thread([hwnd, generation] {
        // File read and ~30 registry queries per type: never on the UI thread.
        // An empty seed is posted too: it tells the UI the layout is loaded
        // and may be saved.
        auto seed = std::make_unique<CtxLayoutSeed>();
        seed->generation = generation;
        app::LoadCtxLayoutFile(app::CtxLayoutCachePath(), CtxLayoutBuild(),
                               app::CtxLayoutFingerprint, seed->layout);
        if (PostMessageW(hwnd, WM_CTX_LAYOUT_SEED, 0, reinterpret_cast<LPARAM>(seed.get())))
            seed.release();
    }).detach();
}

void ApplyCtxLayoutSeed(AppState& s, CtxLayoutSeed& seed) {
    // Read before a registry change: the reload that change started follows.
    if (seed.generation != s.context_menu.cache_generation()) return;
    s.context_menu.MergeComLayout(std::move(seed.layout));
    SaveCtxLayoutIfDirty(s);
}

void SaveCtxLayoutIfDirty(AppState& s) {
    if (!s.context_menu.com_layout_loaded() || !s.context_menu.TakeComLayoutDirty()) return;
    g_ctx_layout_saver.Schedule(s.context_menu.ComLayoutSnapshot());
}

void DiscardCtxLayoutSave() { g_ctx_layout_saver.Discard(); }

// Fired on WM_RBUTTONDOWN (prefetch) and again on menu open (no-op when the
// target is unchanged): pulse_shell starts building the COM menu while the
// Fluent menu fades in.
void StartCtxQuery(AppState& s, std::vector<std::wstring> paths,
                          bool background, const std::wstring& ext) {
    // 网盘程序入口 rows (shell:::{CLSID}) are programs, not files: no shell
    // verbs for them, and no leftover verbs of the previous target either.
    const size_t asked = paths.size();
    std::erase_if(paths, [](const std::wstring& path) { return std::wstring_view(path).starts_with(L"shell:::"); });
    if (asked > 0 && paths.empty()) {
        s.context_menu.Close();
        return;
    }
    s.context_menu.StartQuery(s.ctxMenuPrefs, s.hwnd, std::move(paths),
        background, ext, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        ClipboardPath, [&s](const std::wstring& extension) {
            // A phone's menu is the shell's own: no registry verbs to read.
            if (!std::wstring_view(extension).starts_with(kDeviceVerbKeyPrefix)) PrefetchStaticVerbs(s, extension);
        });
}

// Phone items go to the host as shell ids; their menu rows are cached apart
// from local files of the same type ("device:.jpg").
static bool DeviceCtxTarget(const app::Tab& tab, const std::vector<int>& indices,
                            std::vector<std::wstring>& paths, std::wstring& ext) {
    if (!IsDeviceTab(&tab)) return false;
    paths.clear();
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) continue;
        const std::wstring& id = tab.EntryAt(static_cast<size_t>(index)).shell_id;
        if (id.empty()) {  // a row without its shell id gets no shell rows at all
            paths.clear();
            break;
        }
        paths.push_back(id);
    }
    ext = std::wstring(kDeviceVerbKeyPrefix) + StaticVerbKey(tab, indices);
    return true;
}

void MaybePrefetchHoverCtxMenu(AppState& s) {
    if (s.context_menu.menu_open() || s.safeMode) return;
    if (s.hoverRow < 0) return;
    const ULONGLONG now = GetTickCount64();
    if (s.ctxHoverSince == 0 || now - s.ctxHoverSince < 150) return;
    app::Pane* pane = s.hoverPaneIndex >= 0 ? PaneAtSlot(s, s.hoverPaneIndex) : s.pane;
    app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab || !tab->snapshot || (fs::IsVirtualPath(tab->current_path) && !IsDeviceTab(tab))) return;
    if (s.hoverRow >= tab->CountBound()) return;
    std::wstring path = EntryFullPath(*tab, s.hoverRow);
    if (path.empty() || path == s.ctxHoverPrefetched) return;
    s.ctxHoverPrefetched = path;
    if (std::wstring phone = PcDeviceShellId(*tab, {s.hoverRow}); !phone.empty()) {
        StartCtxQuery(s, {std::move(phone)}, false, std::wstring(kDeviceVerbKeyPrefix) + L"pc");
        return;
    }
    std::vector<std::wstring> paths;
    std::wstring ext;
    if (IsDeviceTab(tab)) {
        DeviceCtxTarget(*tab, tab->IsSelected(s.hoverRow) ? tab->SelectedIndices() : std::vector<int>{s.hoverRow},
                        paths, ext);
    } else if (tab->IsSelected(s.hoverRow)) {
        paths = SelectedFullPaths(*tab);
        ext = StaticVerbKey(*tab, tab->SelectedIndices());
    } else {
        paths.push_back(path);
        ext = StaticVerbKey(*tab, { s.hoverRow });
    }
    if (!paths.empty()) StartCtxQuery(s, std::move(paths), false, ext);
}

// Drain Explorer-menu replies already sitting in the queue. COM handlers are
// slow on a cold load; the fast+complete path is: prefetch on mouse-down,
// reuse the last layout for this extension so mouse-up paints a full menu,
// then swap in this session's live ids when RSP_CTX_ITEMS lands.
void PumpShellMenuMessages(AppState& s) {
    MSG msg{};
    // Static verb reads report to the host window (host_routing.h).
    const HWND verbs_window = ServiceWindow(s);
    while (PeekMessageW(&msg, s.hwnd, WM_SHELLCTX_ITEMS, WM_SHELLCTX_ITEMS, PM_REMOVE) ||
           PeekMessageW(&msg, verbs_window, WM_SHELL_VERBS, WM_SHELL_VERBS, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void WaitForShellMenuReady(AppState& s) {
    s.context_menu.WaitUntilReady(s.hwnd, [&s] { PumpShellMenuMessages(s); });
}

void ScheduleFolderRefresh(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return;
    s.store.MarkDirty(tab->current_path);
    // Follows a shell verb, not F5: new files join the rows on screen (#13).
    RefreshActiveTab(s, RefreshReason::OperationCompleted);
    // Bandizip / 7-Zip often return from InvokeCommand before the archive
    // lands on disk; a second pass catches the late create. Unarmed UNC
    // panes poll once a second from the UI timer.
    s.context_menu.ScheduleFolderRefresh(GetTickCount64());
}

// Static verbs first (they were in the first frame), then COM items. A COM
// has_children header plus its following child rows become one nested entry
// (one-level flyout in the menu).
// First frame (BuildOpenDisplay): every predicted row, flat.
std::vector<ui::FluentMenuItem> ExplorerMenu(
    AppState& s, const std::vector<ui::FluentMenuItem>& base) {
    bool changed = false;
    auto display = s.context_menu.BuildOpenDisplay(s.ctxMenuPrefs, base, changed);
    if (changed) s.ctxMenuPrefs.Save();
    return display;
}

// Later answers are mapped onto the rows on screen, so no shown row moves;
// rows nobody predicted are appended at the bottom. ReplaceItems refuses
// anything else (a visible flyout whose rows changed), and the next answer or
// the next menu catches up.
void RefreshOpenCtxMenu(AppState& s) {
    if (!s.context_menu.menu_open() || !s.menu || !s.menu->IsOpen()) return;
    bool changed = false;
    auto display = s.context_menu.ReconcileDisplay(s.ctxMenuPrefs, changed, GetTickCount64());
    if (changed) s.ctxMenuPrefs.Save();
    const size_t rows = display.size();
    auto shown = display;
    if (s.menu->ReplaceItems(std::move(display))) {
        s.context_menu.CommitDisplay(std::move(shown));
        s.context_menu.NoteDisplayUpdate(rows, GetTickCount64());
    }
}

// Right before TrackPopup: the input message that asked for the menu is the
// last one retrieved, so its time stamp starts the click-to-popup interval.
static void BeginCtxTiming(AppState& s, size_t rows, bool seeded) {
    const DWORD input = static_cast<DWORD>(GetMessageTime());
    s.context_menu.BeginTiming(static_cast<uint32_t>(GetTickCount() - input), rows, seeded,
                               GetTickCount64());
}

static void LogCtxTiming(AppState& s, int cmd) {
    app::CtxMenuTiming timing;
    if (!s.context_menu.TakeTiming(cmd >= app::CmdShellStaticBase, timing)) return;
    try {
        // %LOCALAPPDATA%\Pulse\logs\ctx-menu-<pid>.jsonl, written off the UI thread.
        static SearchRefreshLog sink(L"ctx-menu-", L"PULSE_CTX_TIMING_DIR");
        sink.Push(app::FormatCtxMenuTiming(timing, GetTickCount64()));
    } catch (...) {}
}

// Dispatch for the merged Explorer rows. Returns true when cmd was a shell
// row (or the menu was dismissed) and no built-in dispatch should run.
bool HandleShellMenuCommand(AppState& s, int cmd) {
    return s.context_menu.ExecuteShellCommand(cmd,
        [&s] { WaitForShellMenuReady(s); }, [&s] { ScheduleFolderRefresh(s); });
}

void ShowItemContextMenu(AppState& s, POINT screen_pt, bool from_chip) {
    if(DeferContentSelection(s,[=](AppState& v){ShowItemContextMenu(v,screen_pt,from_chip);})) return;
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    const std::vector<std::wstring> paths =
        tab ? SelectedFullPaths(*tab) : std::vector<std::wstring>{};
    std::vector<std::wstring> device_ids;
    std::wstring device_ext;
    std::wstring pc_device = tab ? PcDeviceShellId(*tab, tab->SelectedIndices()) : std::wstring{};
    if (tab && !paths.empty() && DeviceCtxTarget(*tab, tab->SelectedIndices(), device_ids, device_ext)) {
        if (!device_ids.empty()) StartCtxQuery(s, device_ids, false, device_ext);
        else s.context_menu.Close();
    } else if (!pc_device.empty()) {
        StartCtxQuery(s, {std::move(pc_device)}, false, std::wstring(kDeviceVerbKeyPrefix) + L"pc");
    } else if (tab && !paths.empty() && !IsRecycleTab(tab)) {
        StartCtxQuery(s, paths, false, StaticVerbKey(*tab, tab->SelectedIndices()));
    }
    s.context_menu.SeedComItemsFromCache();
    const bool seeded = s.context_menu.com_predicted();

    std::wstring undoLabel = s.ops.UndoLabel();
    const auto quick_paths = QuickAccessTargets(tab, false);
    auto base_items = BuildFinderItemMenu(s, s.ops.CanUndo(), undoLabel);
    std::vector<std::wstring> window_folders;
    if (!IsRecycleTab(tab)) {
        app::ApplyBuiltinMenuPrefs(base_items, s.ctxMenuPrefs, app::BuiltinMenuSurface::Item);
        if (!paths.empty()) window_folders = WindowTransferFolders(s);
        const app::SelectionCaps caps = SelectionCapsOf(s);
        // Phone items go to another window's folder through Explorer's
        // engine (SubmitDeviceTransfer), like local ones.
        app::AppendWindowTransferCommands(base_items, window_folders, tab && (caps.copy || caps.cut));
    } else {
        app::ApplyBuiltinMenuPrefs(base_items, s.ctxMenuPrefs);
    }
    s.context_menu.OpenMenu(base_items);
    auto display = IsRecycleTab(tab) ? std::move(base_items)
                                     : ExplorerMenu(s, s.context_menu.base_items());
    if (from_chip && !IsRecycleTab(tab)) {
        // Chip ⋯ (prototype app_pins.html): 用 <pinned app> 打开 and
        // 固定到工具栏 ▸ lead the item menu.
        auto apps = ToolbarAppMenuItems(s);
        if (!apps.empty()) {
            apps.back().separator_after = true;
            display.insert(display.begin(), std::make_move_iterator(apps.begin()),
                           std::make_move_iterator(apps.end()));
        }
    }

    if (!IsRecycleTab(tab)) BeginCtxTiming(s, display.size(), seeded);
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    const int favorite_index = cmd >= app::CmdTagBase && cmd < app::CmdTagBase + app::kMaxFavoriteTags
        ? s.places.FavoriteTagIndex(cmd - app::CmdTagBase + 1) : -1;
    s.context_menu.CloseMenu();
    LogCtxTiming(s, cmd);
    if (from_chip && RunToolbarAppMenuCommand(s, cmd)) return;
    if (HandleShellMenuCommand(s, cmd)) return;
    if (HandleQuickAccessCommand(s, cmd, quick_paths)) return;
    const auto window_folder = [&](int base) {
        const int index = cmd - base;
        return index >= 0 && index < static_cast<int>(window_folders.size()) ? &window_folders[static_cast<size_t>(index)]
                                                                              : nullptr;
    };
    if (const std::wstring* folder = window_folder(app::CmdCopyToWindowBase)) TransferSelectionToFolder(s, *folder, false);
    else if (const std::wstring* moved_to = window_folder(app::CmdMoveToWindowBase)) TransferSelectionToFolder(s, *moved_to, true);
    else if (cmd == app::CmdTags) ShowTagPicker(s, screen_pt);
    else if (favorite_index >= 0)
        ToggleTagForSelection(s, s.places.tags[static_cast<size_t>(favorite_index)].id, paths);
    else if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

namespace {
// The 新建 entries, shared with the toolbar New menu (defined below).
std::vector<app::sysctx::NewTemplate> NewTemplatesForMenu(AppState& s, bool wait = true);
bool RunShellNewCommand(AppState& s, int cmd, const std::vector<app::sysctx::NewTemplate>& templates);
void AppendBackgroundNewMenu(std::vector<ui::FluentMenuItem>& items,
                             const std::vector<app::sysctx::NewTemplate>& templates);
} // namespace

void ShowBackgroundContextMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    app::BackgroundViewOptions view_options;
    view_options.view_mode = tab->view_mode;
    view_options.wall = tab->wall;
    view_options.sort_column = tab->sort_column;
    view_options.sort_direction = tab->sort_direction;
    view_options.folder_sort = static_cast<int>(tab->EffectiveFolderSort());
    view_options.details_panel = s.showDetailsPanel;
    view_options.can_sort = kind != L"starred" && kind != L"recent";
    view_options.indexed_search = (kind == L"search" || kind == L"saved-search") && !tab->content_results;
    view_options.show_path = kind == L"search" || kind == L"saved-search" || kind == L"recycle";
    view_options.filesystem = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    view_options.group_virtual = kind == L"search" || kind == L"saved-search" || app::IsTagViewKind(kind) || kind == L"recycle";
    view_options.can_group = !tab->content_results &&
        (view_options.filesystem || kind == L"recent" || view_options.group_virtual);
    view_options.group_by = tab->group_by;
    view_options.can_apply_group_all = view_options.can_group && view_options.filesystem;
    if (IsRecycleTab(tab)) {
        const bool can_empty = tab->snapshot && tab->EntryCount() != 0;
        const std::wstring undoLabel = s.ops.UndoLabel();
        auto items = app::BuildRecycleBackgroundMenu(s.ops.CanUndo(), undoLabel, can_empty);
        app::AppendBackgroundViewCommands(items, view_options);
        s.context_menu.OpenMenu(std::move(items));
        const int cmd = s.menu->TrackPopup(screen_pt, s.context_menu.base_items());
        s.context_menu.CloseMenu();
        if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
        return;
    }
    const std::wstring device_folder_id =
        IsDeviceTab(tab) ? app::DeviceRegistry::Instance().FolderShellId(tab->current_path) : std::wstring();
    if (!device_folder_id.empty()) {
        // A phone folder's background menu is the shell's own.
        StartCtxQuery(s, { device_folder_id }, true, std::wstring(kDeviceVerbKeyPrefix) + ipc::kBackgroundVerbKey);
        s.context_menu.SeedComItemsFromCache();
    } else if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
        StartCtxQuery(s, { tab->current_path }, true, ipc::kBackgroundVerbKey);
        s.context_menu.SeedComItemsFromCache();
    } else {
        // This PC and Pulse views have no shell folder to ask: drop the last
        // item's Explorer rows (格式化 under This PC's background otherwise).
        s.context_menu.Close();
    }
    const bool seeded = s.context_menu.com_predicted();

    std::wstring undoLabel = s.ops.UndoLabel();
    bool canPaste = !s.tray.batches().empty() || ClipboardHasFiles();
    const auto recent_changes_path = app::RecentChangesMenuPath(*tab, true);
    auto base_items = app::BuildBackgroundMenu(canPaste, s.ops.CanUndo(), undoLabel);
    app::AppendRecentChangesCommand(base_items, recent_changes_path);
    app::AppendBackgroundViewCommands(base_items, view_options);
    ApplyWorkspacePinLabel(base_items, s);
    if (!CanPinNetwork(s)) EraseCommand(base_items, app::CmdPinNetwork);
    // This PC has no path of its own to copy (Explorer offers none there).
    if (tab->current_path.empty()) EraseCommand(base_items, app::CmdCopyPath);
    const auto quick_paths = QuickAccessTargets(tab, true);
    AppendQuickAccessCommand(s, base_items, quick_paths);
    app::ApplyBuiltinMenuPrefs(base_items, s.ctxMenuPrefs, app::BuiltinMenuSurface::Background);
    const bool can_create_here = CanCreateHere(tab);
    const auto new_templates = can_create_here ? NewTemplatesForMenu(s, false)
                                               : std::vector<app::sysctx::NewTemplate>{};
    if (can_create_here) AppendBackgroundNewMenu(base_items, new_templates);
    // This PC, a server's share list, the Network view and other Pulse views
    // take no new items or pastes, and have no folder of their own to open a
    // terminal in or pin as a workspace.
    if (!can_create_here) {
        // A writable phone folder takes new folders (not empty files).
        const bool phone_folder = IsDeviceTab(tab) && app::DeviceRegistry::Instance().FolderWritable(tab->current_path);
        EraseCommand(base_items, app::CmdNewTextFile);
        if (!phone_folder) EraseCommand(base_items, app::CmdNewFolder);
        if (!CanPasteHere(tab)) EraseCommand(base_items, app::CmdPaste);  // a phone folder takes pastes
    }
    if (!view_options.filesystem || app::IsUncServerRoot(tab->current_path)) {
        for (int command : {app::CmdOpenTerminal, app::CmdPinWorkspace}) EraseCommand(base_items, command);
    }
    if (tab->current_path.empty()) {  // This PC, as in Explorer
        auto map = VerbRow(app::CmdMapNetworkDrive, l10n::Get(l10n::StringId::MapNetworkDrive).c_str(), L"\xE8CE");
        map.separator_after = true;
        base_items.insert(base_items.begin(), std::move(map));
    } else if (IsConnectServerPage(tab->current_path)) {  // the network page, a server's shares
        auto connect = VerbRow(app::CmdConnectServer, l10n::Get(l10n::StringId::ConnectServerMenu).c_str(), L"\xE968");
        auto map = VerbRow(app::CmdMapNetworkDrive, l10n::Get(l10n::StringId::MapNetworkDrive).c_str(), L"\xE8CE");
        map.separator_after = true;
        base_items.insert(base_items.begin(), std::move(map));
        base_items.insert(base_items.begin(), std::move(connect));
    }
    // In-app Git: pull / push / refresh for a folder inside a repository.
    const std::wstring git_root = tab->git_root;
    if (view_options.filesystem) AppendGitMenu(*tab, base_items);
    s.context_menu.OpenMenu(std::move(base_items));
    auto display = ExplorerMenu(s, s.context_menu.base_items());

    BeginCtxTiming(s, display.size(), seeded);
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    s.context_menu.CloseMenu();
    LogCtxTiming(s, cmd);
    if (HandleShellMenuCommand(s, cmd)) return;
    if (HandleQuickAccessCommand(s, cmd, quick_paths)) return;
    if (RunShellNewCommand(s, cmd, new_templates)) return;
    if (cmd == app::CmdViewRecentChanges) { OpenChangeView(s, recent_changes_path); return; }
    if (cmd == app::CmdMapNetworkDrive) { RunMapNetworkDrive(s, L""); return; }
    if (HandleGitCommand(s, cmd, git_root)) return;
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

namespace {
struct ToolbarToolDef {
    int command;   // toggles: none, they run as CmdToolbarToolBase + index
    const wchar_t* glyph;
    l10n::StringId label;
    const wchar_t* shortcut;
    bool toggle;
};
// The tools AppPrefs::toolbar_tools can pin, in app::kToolbarToolIds order
// (新建文件夹 is the New button itself and always shown). Toggles flip an
// existing setting; the toolbar shows them in the accent color while on.
constexpr ToolbarToolDef kToolbarTools[] = {
    {app::CmdNewTextFile, L"\xE8A5", l10n::StringId::NewTextDocument, L"", false},
    {app::CmdCopyPath, ui::command_icons::kGlyphCopyPath, l10n::StringId::CopyPath, L"Ctrl+Shift+C", false},
    {app::CmdOpenTerminal, L"\xE756", l10n::StringId::OpenInTerminal, L"", false},
    {app::CmdProperties, L"\xE946", l10n::StringId::Properties, L"Alt+Enter", false},
    {app::CmdUndo, L"\xE7A7", l10n::StringId::Undo, L"Ctrl+Z", false},
    {0, L"\xE7B3", l10n::StringId::SettingsShowHidden, L"", true},
    {0, L"\xEA18", l10n::StringId::SettingsShowProtected, L"", true},
    {0, L"\xE73F", l10n::StringId::SettingsDensityCompact, L"", true},
    {0, L"\xE8BF", l10n::StringId::ListZebraRows, L"", true},
    {0, L"\xE9D2", l10n::StringId::ListSizeBar, L"", true},
    {0, L"\xEC92", l10n::StringId::ListSmartDate, L"", true},
    {0, L"\xE739", l10n::StringId::ListSelectionOutline, L"", true},
    {0, L"\xE90C", l10n::StringId::HelpSidebar, L"Ctrl+B", true},
    {0, L"\xE793", l10n::StringId::TooltipToggleTheme, L"", true},
    // T3d: the former fixed buttons, then the rest of the prototype's tools.
    {app::CmdCut, L"\xE8C6", l10n::StringId::Cut, L"Ctrl+X", false},
    {app::CmdCopy, L"\xE8C8", l10n::StringId::Copy, L"Ctrl+C", false},
    {app::CmdPaste, L"\xE77F", l10n::StringId::Paste, L"Ctrl+V", false},
    {app::CmdRename, L"\xE8AC", l10n::StringId::Rename, L"F2", false},
    {app::CmdDelete, L"\xE74D", l10n::StringId::Delete, L"Delete", false},
    {0, ui::kToolbarSplitGlyph, l10n::StringId::SplitLayout, L"", false},       // opens the split menu
    {app::CmdDetailsPanel, L"\xE8A0", l10n::StringId::DetailsPane, L"", true},
    {app::CmdColumnLayout, ui::kToolbarColumnsGlyph, l10n::StringId::ColumnLayout, L"", true},
    {app::CmdSelectAll, L"\xE8B3", l10n::StringId::SelectAll, L"Ctrl+A", false},
    {app::CmdInvertSelection, L"\xE8AB", l10n::StringId::InvertSelection, L"", false},
    {app::CmdSelectWildcard, ui::command_icons::kGlyphSelectWild, l10n::StringId::HelpSelectPattern, L"", false},
    {app::CmdNewMarkdownFile, L"\xE943", l10n::StringId::MarkdownDocument, L"", false},
    {app::CmdViewRecentChanges, L"\xE81C", l10n::StringId::ToolbarSelRecent, L"", false},
    {app::CmdApplyViewToAllFolders, ui::command_icons::kGlyphApplyAll, l10n::StringId::ToolbarToolApplyAll, L"", false},
    {app::CmdSearchAll, L"\xE774", l10n::StringId::ToolbarToolSearchAll, L"", false},
    {app::CmdAdvancedSearch, ui::command_icons::kGlyphAdvSearch, l10n::StringId::AdvancedSearch, L"", false},
    {app::CmdOpenRecycle, L"\xE75C", l10n::StringId::RecycleBin, L"", false},
    {app::CmdMapNetworkDrive, L"\xE8CE", l10n::StringId::MapNetworkDrive, L"", false},
    {app::CmdShortcutHelp, L"\xE765", l10n::StringId::KeyboardShortcuts, L"", false},
    {app::CmdSettings, L"\xE713", l10n::StringId::Settings, L"", false},
    {app::CmdCompressZip, L"\xF012", l10n::StringId::ToolbarToolZip, L"", false},
    {app::CmdToggleFolderSizes, L"\xEDA2", l10n::StringId::ToolbarToolFolderSizes, L"", true},
    {0, L"\xE7C3", l10n::StringId::ToolbarToolExtensions, L"", true},
};
static_assert(std::size(kToolbarTools) == app::kToolbarToolCount, "one catalog row per toolbar_tools.h id");

// The selection chip's actions AppPrefs::toolbar_selection can keep, in
// app::kSelectionActionIds order. `hint` names the condition under which the
// item menu offers the row; `tool` is the same command on the tools page.
enum SelectionHint { kSelHintNone, kSelHintMulti, kSelHintTwoFolders, kSelHintFolders, kSelHintSearch };
struct SelectionActionDef {
    int command;
    int alt_command;
    const wchar_t* glyph;
    l10n::StringId label;
    SelectionHint hint;
    const wchar_t* tool;
};
constexpr SelectionActionDef kSelectionActions[] = {
    {app::CmdBatchRename, 0, ui::command_icons::kGlyphBatchRename, l10n::StringId::BatchRename, kSelHintMulti, nullptr},
    {app::CmdGroupIntoFolder, 0, ui::command_icons::kGlyphGroupFolder, l10n::StringId::NewFolderWithSelection_2, kSelHintNone, nullptr},
    {app::CmdDissolveFolder, 0, L"\xED25", l10n::StringId::UngroupFolder_2, kSelHintFolders, nullptr},
    {app::CmdCompareSideBySide, 0, ui::command_icons::kGlyphCompare, l10n::StringId::CompareSideBySide, kSelHintTwoFolders, nullptr},
    {app::CmdTags, 0, L"\xE8EC", l10n::StringId::TagsEllipsis, kSelHintNone, nullptr},
    {app::CmdCopyPath, 0, ui::command_icons::kGlyphCopyPath, l10n::StringId::CopyPath, kSelHintNone, L"copy_path"},
    {app::CmdOpenInNewTab, 0, L"\xE8A7", l10n::StringId::OpenNewTab, kSelHintFolders, nullptr},
    {app::CmdOpenPath, 0, ui::command_icons::kGlyphOpenPath, l10n::StringId::OpenPath, kSelHintSearch, nullptr},
    {app::CmdPinQuickAccess, app::CmdUnpinQuickAccess, ui::command_icons::kGlyphPinQuickAccess, l10n::StringId::SidebarQuickAccess, kSelHintNone, nullptr},
    {app::CmdProperties, 0, L"\xE946", l10n::StringId::Properties, kSelHintNone, L"properties"},
    {app::CmdOpenTerminal, 0, L"\xE756", l10n::StringId::OpenInTerminal, kSelHintNone, L"terminal"},
    {app::CmdViewRecentChanges, 0, L"\xE81C", l10n::StringId::ToolbarSelRecent, kSelHintNone, L"recent"},
    {app::CmdPinWorkspace, 0, ui::command_icons::kGlyphPinWorkspace, l10n::StringId::PinWorkspace, kSelHintNone, nullptr},
    {app::CmdPinNetwork, 0, ui::command_icons::kGlyphPinNetwork, l10n::StringId::PinNetwork, kSelHintNone, nullptr},
};
static_assert(std::size(kSelectionActions) == app::kSelectionActionCount, "one catalog row per toolbar_selection.h id");

// One shape per command (toolbar redesign §7): two different commands on the
// tools page or the selection chip never draw the same glyph.
constexpr bool SameGlyph(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr bool ToolbarGlyphsDistinct() {
    for (size_t i = 0; i < std::size(kToolbarTools); ++i)
        for (size_t j = i + 1; j < std::size(kToolbarTools); ++j)
            if (SameGlyph(kToolbarTools[i].glyph, kToolbarTools[j].glyph)) return false;
    for (size_t i = 0; i < std::size(kSelectionActions); ++i) {
        for (size_t j = i + 1; j < std::size(kSelectionActions); ++j)
            if (SameGlyph(kSelectionActions[i].glyph, kSelectionActions[j].glyph)) return false;
        for (const ToolbarToolDef& tool : kToolbarTools)
            if (tool.command != kSelectionActions[i].command && SameGlyph(tool.glyph, kSelectionActions[i].glyph))
                return false;
    }
    return true;
}
static_assert(ToolbarGlyphsDistinct(), "every toolbar command has its own glyph");
// Offered because of the moment rather than chosen by the user (toolbar
// redesign §5): drive / network / Recycle Bin verbs, split, two folders,
// several items, search results.
bool SituationalSelectionCommand(int command) {
    switch (command) {
    case app::CmdUnlockDrive: case app::CmdEjectDrive: case app::CmdDisconnectDrive: case app::CmdRestoreRecycle:
    case app::CmdMapNetworkDrive: case app::CmdUnpinNetwork: case app::CmdCopyToTarget: case app::CmdMoveToTarget:
    case app::CmdCompareSideBySide: case app::CmdBatchRename: case app::CmdOpenPath: case app::CmdForgetNetworkRecent:
    case app::CmdRenameNetwork:
        return true;
    default:
        return false;
    }
}
int SelectionActionFor(int command) {
    for (size_t i = 0; i < std::size(kSelectionActions); ++i)
        if (kSelectionActions[i].command == command || (kSelectionActions[i].alt_command && kSelectionActions[i].alt_command == command))
            return static_cast<int>(i);
    return -1;
}
// Menu labels end in "..." where a dialog follows; tiles and chips drop it.
std::wstring WithoutEllipsis(std::wstring label) {
    for (const wchar_t* tail : {L"...", L"\u2026"}) {
        const std::wstring_view t{tail};
        if (label.size() > t.size() && std::wstring_view{label}.ends_with(t)) label.resize(label.size() - t.size());
    }
    return label;
}
// Tools with a command of their own run it (details / columns too, which are
// toggles); the others (the setting switches, split) run as
// CmdToolbarToolBase + index, kept below CmdNewPrimaryBase.
int ToolbarToolCommand(size_t index) {
    return kToolbarTools[index].command ? kToolbarTools[index].command
                                        : app::CmdToolbarToolBase + static_cast<int>(index);
}
bool ToolbarToolPinned(const AppState& s, std::wstring_view id) {
    for (const auto& kept : s.appPrefs.toolbar_tools) if (kept == id) return true;
    return false;
}
// A toggle tool's setting, as the toolbar shows it.
bool ToolbarToggleOn(const AppState& s, size_t index) {
    const std::wstring_view id = app::kToolbarToolIds[index];
    const app::AppPrefs& p = s.appPrefs;
    if (id == L"hidden") return p.show_hidden_files;
    if (id == L"extensions") return p.show_file_extensions;
    if (id == L"protected") return p.show_protected_os_files;
    if (id == L"compact") return p.row_height <= 28;
    if (id == L"zebra") return p.list_zebra_rows;
    if (id == L"size_bar") return p.list_size_bar;
    if (id == L"smart_date") return p.list_smart_date;
    if (id == L"outline") return p.list_selection_outline;
    if (id == L"sidebar") return !p.sidebar_collapsed;  // lit = sidebar shown
    if (id == L"theme") return s.darkMode;
    if (id == L"details") return s.showDetailsPanel;
    if (id == L"folder_size") return p.folder_size_auto;
    if (id == L"columns") { const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr; return tab && tab->column_layout; }
    return false;
}
ui::FluentMenuItem CustomizeToolbarItem() {
    ui::FluentMenuItem item;
    item.command = app::CmdCustomizeToolbar;
    item.text = l10n::Get(l10n::StringId::CustomizeToolbarMenu);
    item.glyph = L"\xE9E9";   // Sliders: arrange the toolbar (not the pin of 固定)
    return item;
}
} // namespace

namespace {
// Explorer's 新建 entries for the New menu. A full HKCR walk takes a moment,
// so the first menu waits for it once; later menus show the last list and
// refresh it in the background (installs / the system page show up next time).
struct NewTemplateCache {
    std::mutex mutex;
    std::vector<app::sysctx::NewTemplate> list;
    bool loaded = false;
    std::atomic<bool> refreshing{false};
};
NewTemplateCache& NewTemplates() {
    static NewTemplateCache cache;
    return cache;
}

std::vector<app::sysctx::NewTemplate> LoadNewTemplates() {
    app::sysctx::Roots roots;
    roots.check_owner = false;
    return app::sysctx::ListNewTemplates(roots);
}

// wait=false (the background menu, which must open at once): before the first
// load has finished, no system rows this time and the load runs in the back.
std::vector<app::sysctx::NewTemplate> NewTemplatesForMenu(AppState& s, bool wait) {
    NewTemplateCache& cache = NewTemplates();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.loaded || !wait) {
            if (!cache.refreshing.exchange(true)) {
                s.worker.EnqueueIo([] {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    auto fresh = LoadNewTemplates();
                    for (const auto& t : fresh) app::FileTypeMenuIcon(t.ext);
                    NewTemplateCache& c = NewTemplates();
                    {
                        std::lock_guard<std::mutex> l(c.mutex);
                        c.list = std::move(fresh);
                        c.loaded = true;
                    }
                    c.refreshing = false;
                    if (SUCCEEDED(com)) CoUninitialize();
                });
            }
            return cache.list;
        }
    }
    auto list = LoadNewTemplates();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.list = list;
    cache.loaded = true;
    return list;
}

void LaunchNewCommand(const std::wstring& command_line, const std::wstring& dir) {
    wchar_t expanded[2048]{};
    const DWORD n = ExpandEnvironmentStringsW(command_line.c_str(), expanded, ARRAYSIZE(expanded));
    std::wstring line = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : command_line;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// One 新建 entry, the way Explorer makes it.
void CreateFromNewTemplate(AppState& s, const app::sysctx::NewTemplate& t) {
    app::Tab* tab = ActiveTab(s);
    if (!CanCreateHere(tab)) return;
    const std::wstring dir = tab->current_path;
    const std::wstring stem = TagLabel(l10n::StringId::NewItemNamed, t.display);
    if (_wcsicmp(t.ext.c_str(), L".lnk") == 0) {
        // Explorer's shortcut creator: an empty .lnk (Explorer's 新建快捷方式
        // name) handed to the classic wizard, which renames it on 完成 and
        // removes it on 取消.
        const std::wstring lnk_stem = l10n::Get(l10n::StringId::NewShortcutName);
        const std::wstring full = JoinChild(dir, app::UniqueChildName(dir, lnk_stem, t.ext));
        HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        CloseHandle(file);
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, ARRAYSIZE(system));
        LaunchNewCommand(L"\"" + std::wstring(system) + L"\\rundll32.exe\" appwiz.cpl,NewLinkHere " + full, dir);
        return;
    }
    if (!t.command.empty() && t.template_file.empty() && t.data.empty() && !t.null_file) {
        // Command-only entries (联系人 …) create the file themselves.
        const std::wstring full = JoinChild(dir, app::UniqueChildName(dir, stem, t.ext));
        std::wstring line = t.command;
        const size_t at = line.find(L"%1");
        if (at != std::wstring::npos) line.replace(at, 2, full);
        else line += L" \"" + full + L"\"";
        LaunchNewCommand(line, dir);
        return;
    }
    CreateNewFileItem(s, stem, t.ext, t.template_file, t.data);
}

// Explorer's 新建 entries as menu rows: 快捷方式… joins Pulse's own rows, the
// file types (with the icons Explorer shows) follow after a separator.
void AppendShellNewRows(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<app::sysctx::NewTemplate>& templates, bool can_create) {
    std::vector<ui::FluentMenuItem> system_rows;
    for (size_t i = 0; i < templates.size() && i < static_cast<size_t>(app::kMaxShellNewItems); ++i) {
        const auto& t = templates[i];
        // Pulse's own rows already cover these.
        if (_wcsicmp(t.ext.c_str(), L".txt") == 0 || _wcsicmp(t.ext.c_str(), L".md") == 0) continue;
        ui::FluentMenuItem row;
        row.command = app::CmdShellNewBase + static_cast<int>(i);
        row.enabled = can_create;
        if (_wcsicmp(t.ext.c_str(), L".lnk") == 0) {
            row.text = t.display + L"\u2026";
            row.glyph = L"\xE71B";
            items.push_back(std::move(row));
            continue;
        }
        row.text = t.display;
        row.icon = app::FileTypeMenuIcon(t.ext);
        row.glyph = L"\xE8A5";
        system_rows.push_back(std::move(row));
    }
    if (!system_rows.empty()) {
        if (!items.empty()) items.back().separator_after = true;
        for (auto& row : system_rows) items.push_back(std::move(row));
    }
}

bool RunShellNewCommand(AppState& s, int cmd, const std::vector<app::sysctx::NewTemplate>& templates) {
    if (cmd < app::CmdShellNewBase || cmd >= app::CmdShellNewBase + app::kMaxShellNewItems) return false;
    const size_t index = static_cast<size_t>(cmd - app::CmdShellNewBase);
    if (index < templates.size()) CreateFromNewTemplate(s, templates[index]);
    return true;
}

// The background menu's 新建 flyout: the toolbar New menu's rows, minus the
// flat 新建文件夹 / 新建文本文档 rows the menu already shows (those stay
// separately configurable on the 右键菜单 page), right after them.
void AppendBackgroundNewMenu(std::vector<ui::FluentMenuItem>& items,
                             const std::vector<app::sysctx::NewTemplate>& templates) {
    bool has_folder = false, has_text = false;
    size_t at = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].command == app::CmdNewFolder) { has_folder = true; at = i + 1; }
        if (items[i].command == app::CmdNewTextFile) { has_text = true; at = i + 1; }
    }
    ui::FluentMenuItem header;
    header.command = app::CmdNone;
    header.text = l10n::Get(l10n::StringId::New);
    header.glyph = L"\xE710";
    for (auto& row : app::BuildNewMenu()) {
        if ((row.command == app::CmdNewFolder && has_folder) || (row.command == app::CmdNewTextFile && has_text))
            continue;
        header.children.push_back(std::move(row));
    }
    AppendShellNewRows(header.children, templates, true);
    if (header.children.empty()) return;
    header.children.back().separator_after = false;
    if (at > 0) {
        header.separator_after = items[at - 1].separator_after;
        items[at - 1].separator_after = false;
    }
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(at), std::move(header));
}
} // namespace

void ShowNewDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto anchor = s.renderer.NewCommandRect((float)s.compositor.Width()); POINT pt{(LONG)anchor.left, (LONG)anchor.bottom};
    ClientToScreen(s.hwnd, &pt);
    auto items = app::BuildNewMenu();
    // Prototype: Pulse's types, 快捷方式…, then what the system 新建 menu
    // offers (with the file type icons Explorer shows), then 自定义工具栏.
    const auto templates = NewTemplatesForMenu(s);
    const app::Tab* active = ActiveTab(s);
    const bool can_create = CanCreateHere(active);
    // A writable phone folder takes new folders only, not files (CreateDeviceFolder).
    const bool phone_folder = !can_create && IsDeviceTab(active) &&
        app::DeviceRegistry::Instance().FolderWritable(active->current_path);
    for (auto& item : items) item.enabled = can_create || (phone_folder && item.command == app::CmdNewFolder);
    AppendShellNewRows(items, templates, can_create);
    items.back().separator_after = true;
    // 「新建」按钮的主操作 (was a section of the old customize sheet).
    ui::FluentMenuItem primary;
    primary.text = l10n::Get(l10n::StringId::NewButtonPrimary);
    primary.glyph = L"\xE710";
    const l10n::StringId choices[] = {l10n::StringId::NewPrimaryFolder, l10n::StringId::NewPrimaryLast,
                                      l10n::StringId::NewPrimaryMenu};
    for (int i = 0; i < 3; ++i) {
        ui::FluentMenuItem row;
        row.command = app::CmdNewPrimaryBase + i;
        row.text = l10n::Get(choices[i]);
        row.radio = s.appPrefs.toolbar_new_primary == i;
        row.radio_group = true;
        primary.children.push_back(std::move(row));
    }
    items.push_back(std::move(primary));
    items.push_back(CustomizeToolbarItem());
    {
        // Motion v2 "newmenu": the menu grows out of the 新建 button.
        POINT tl{(LONG)anchor.left, (LONG)anchor.top}, br{(LONG)anchor.right, (LONG)anchor.bottom};
        ClientToScreen(s.hwnd, &tl);
        ClientToScreen(s.hwnd, &br);
        s.menu->SetMorphFrom(RECT{tl.x, tl.y, br.x, br.y});
    }
    int cmd = s.menu->TrackPopup(pt, std::move(items));
    if (RunShellNewCommand(s, cmd, templates)) return;
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Stage 1B-2: OLE drag & drop wiring.
void ShowSplitDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    // Under the split pin when it is on the toolbar (T3d), else under More.
    const float width = static_cast<float>(s.compositor.Width());
    const float left = s.renderer.EffectiveSidebarWidth(width);
    const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale),
                                                   s.pane ? s.pane->filter_expand : 0.0f);
    D2D1_RECT_F anchor = layout.overflow;
    size_t slot = 0;
    for (const auto& id : s.appPrefs.toolbar_tools) {
        if (app::ToolbarToolIndex(id) < 0) continue;
        if (id == L"split") {
            if (slot < layout.pins.size() && layout.pins[slot].right > layout.pins[slot].left) anchor = layout.pins[slot];
            break;
        }
        ++slot;
    }
    POINT pt{(LONG)anchor.left, (LONG)anchor.bottom};
    ClientToScreen(s.hwnd, &pt);
    auto items = app::BuildSplitMenu(static_cast<int>(LayoutOf(s)));
    if (!items.empty()) items.back().separator_after = true;
    const app::LayoutTab* lt = s.window_tabs.Active();
    ui::FluentMenuItem compare;
    compare.command = app::CmdCompareToggle;
    compare.text = l10n::Get(l10n::StringId::CompareMenu);
    compare.glyph = L"\xE89F";
    compare.checked = lt && lt->compare;
    compare.enabled = FolderCompareAvailable(s);
    items.push_back(std::move(compare));
    if (lt && lt->compare) {
        ui::FluentMenuItem diff;
        diff.command = app::CmdCompareDiffOnly;
        diff.text = l10n::Get(l10n::StringId::CompareDiffOnly);
        diff.glyph = L"\xE71C"; // filter
        diff.checked = lt->compare_diff_only;
        items.push_back(std::move(diff));
    }
    int cmd = s.menu->TrackPopup(pt, std::move(items));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Browser-style tab groups: named + colored; strip chips open the group popup.
// ---------------------------------------------------------------------------
void RefreshSidebarModel(AppState& s) {
    RequestSidebarRefresh(s, true);
    if (s.networkLocations) RequestNetworkLocations(s);
}

void ShowDeviceHelp(AppState& s) {
    ui::ConfirmDialogSpec spec;
    spec.tone = ui::ConfirmTone::Info;
    spec.glyph = L"\xE8EA";  // CellPhone
    spec.title = l10n::Get(l10n::StringId::DeviceHelpTitle).c_str();
    const l10n::StringId steps[] = {l10n::StringId::DeviceHelpStep1, l10n::StringId::DeviceHelpStep2,
                                    l10n::StringId::DeviceHelpStep3};
    // One wrapped paragraph per step: card rows are one line, these are sentences.
    for (int i = 0; i < 3; ++i) {
        if (i) spec.message += L"\n";
        spec.message += std::to_wstring(i + 1) + L". " + l10n::Get(steps[i]).c_str();
    }
    spec.note = l10n::Get(l10n::StringId::DeviceHelpNote).c_str();
    spec.confirm_text = l10n::Get(l10n::StringId::TeachGotIt).c_str();
    spec.single_button = true;
    (void)ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
}

app::SidebarEntry* QuickAccessEntryForPath(AppState& s,
                                                   const std::wstring& path) {
    for (auto* group : { &s.sidebar.quick_access, &s.sidebar.cloud, &s.sidebar.starred }) {
        for (auto& entry : *group) {
            if (entry.path.empty() || entry.path == app::MakeRecyclePath()) continue;
            if (_wcsicmp(entry.path.c_str(), path.c_str()) == 0)
                return &entry;
        }
    }
    return nullptr;
}

void ShowStarredBadgeEditor(AppState& s, const std::wstring& path,
                                    POINT screen_pt) {
    const app::StarredItem* initial = s.places.FindStarred(path);
    // Quick-access badges are stored in the places catalog: the sidebar model
    // is rebuilt on restart, language and volume changes (#41).
    const bool quick_access = !initial && QuickAccessEntryForPath(s, path);
    if ((!initial && !quick_access) || !EnsureMenu(s)) return;
    constexpr int kCustomColor = 30100;
    const app::QuickAccessBadge* saved = quick_access ? s.places.FindQuickAccessBadge(path) : nullptr;
    uint32_t color = initial ? initial->badge_rgb : saved ? saved->badge_rgb : app::kDefaultBadgeRgb;
    std::wstring current_text = initial ? initial->badge : saved ? saved->badge : std::wstring();
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::BadgeTextHint));
    s.menu->SetInitialFilterText(current_text);
    s.menu->SetFilterMinWidth(260.0f);
    auto build = [&](const std::wstring& query) {
        current_text = query.substr(0, 12);
        if (initial) s.places.SetStarredBadge(path, query, color);
        else s.places.SetQuickAccessBadge(path, query, color);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem strip;
        const auto& palette = TagColorPalette(s);
        for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
            ui::FluentMenuSwatch sw;
            sw.command = app::CmdTabColorBase + i;
            sw.color = ui::HexColor(palette[static_cast<size_t>(i)]);
            sw.checked = color == palette[static_cast<size_t>(i)];
            strip.quick_swatches.push_back(sw);
        }
        items.push_back(std::move(strip));
        ui::FluentMenuItem custom;
        custom.command = kCustomColor;
        custom.text = l10n::Get(l10n::StringId::CustomColor);
        custom.has_swatch = true;
        custom.swatch_color = ui::HexColor(color);
        custom.separator_after = true;
        items.push_back(std::move(custom));
        return items;
    };
    auto apply_color = [&](uint32_t next) {
        color = next;
        if (initial) s.places.SetStarredBadge(path, current_text, color);
        else s.places.SetQuickAccessBadge(path, current_text, color);
    };
    for (;;) {
        const int cmd = s.menu->TrackPopup(screen_pt,
            build(initial ? initial->badge : current_text),
            [&](const std::wstring& query) { return build(query); });
        const auto& palette = TagColorPalette(s);
        if (cmd >= app::CmdTabColorBase &&
            cmd < app::CmdTabColorBase + static_cast<int>(palette.size())) {
            apply_color(palette[static_cast<size_t>(cmd - app::CmdTabColorBase)]);
            break;
        }
        if (cmd == kCustomColor) {
            uint32_t picked = color;
            if (ui::ColorPickerPopup::Pick(s.hwnd, &s.compositor, s.menu.get(),
                                           s.scale, screen_pt, picked, s.darkMode, picked)) {
                apply_color(picked);
                AppendCustomTagColor(s, picked);
                continue;
            }
        }
        break;
    }
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowCuratedItemMenu(AppState& s, const std::wstring& path,
                                bool recent, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    constexpr int kCustomColor = 30100;
    // 收藏夹分组与分隔线: 放进分组 › (each group, 新建分组…, 移出分组) and the
    // separator after it.
    constexpr int kNewGroup = 30190, kLeaveGroup = 30191, kSeparator = 30192, kGroupBase = 30200;
    constexpr int kOpenNewTab = 30180;
    std::vector<std::wstring> group_ids;
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem open;
    open.command = app::CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8A0";
    items.push_back(std::move(open));
    // #58-2: folder places (quick access, starred folders) also open in a new
    // tab; the shortcut column names the mouse gestures for it.
    if (const app::StarredItem* place = s.places.FindStarred(path);
        !recent && ((place && place->kind == app::PlaceItemKind::Folder) || QuickAccessEntryForPath(s, path))) {
        ui::FluentMenuItem new_tab;
        new_tab.command = kOpenNewTab;
        new_tab.text = l10n::Get(l10n::StringId::OpenNewTab);
        new_tab.shortcut = l10n::Get(l10n::StringId::QuickAccessNewTabHint);
        new_tab.glyph = L"\xE8A7";
        items.push_back(std::move(new_tab));
    }
    if (recent) {
        ui::FluentMenuItem remove;
        remove.command = app::CmdRemoveRecent;
        remove.text = l10n::Get(l10n::StringId::RemoveRecent);
        remove.glyph = L"\xE711";
        items.push_back(std::move(remove));
    } else {
        ui::FluentMenuItem badge;
        badge.command = app::CmdEditStarBadge;
        badge.text = l10n::Get(l10n::StringId::EditBadge);
        badge.glyph = L"\xE8D2";
        items.push_back(std::move(badge));
        const app::StarredItem* starred = s.places.FindStarred(path);
        const app::QuickAccessBadge* quick_badge = s.places.FindQuickAccessBadge(path);
        const uint32_t badge_rgb = starred ? starred->badge_rgb
            : quick_badge ? quick_badge->badge_rgb : app::kDefaultBadgeRgb;
        const auto& palette = TagColorPalette(s);
        ui::FluentMenuItem colors;
        colors.command = app::CmdNone;
        colors.quick_swatches.reserve(palette.size());
        for (size_t i = 0; i < palette.size(); ++i) {
            ui::FluentMenuSwatch swatch;
            swatch.command = app::CmdTabColorBase + static_cast<int>(i);
            swatch.color = ui::HexColor(palette[i]);
            swatch.checked = badge_rgb == palette[i];
            colors.quick_swatches.push_back(std::move(swatch));
        }
        colors.separator_after = true;
        items.push_back(std::move(colors));
        ui::FluentMenuItem custom;
        custom.command = kCustomColor;
        custom.text = l10n::Get(l10n::StringId::CustomColor);
        custom.has_swatch = true;
        custom.swatch_color = ui::HexColor(badge_rgb);
        custom.separator_after = true;
        items.push_back(std::move(custom));
        if (starred && starred->kind != app::PlaceItemKind::File) {
            ui::FluentMenuItem move;
            move.text = l10n::Get(l10n::StringId::FavoritesGroupMoveTo);
            move.glyph = L"\xE8F1";
            for (const auto& group : s.places.starred_groups) {
                if (group_ids.size() >= 90) break;
                // An emptied group lingers for the snack's undo; it is not offered.
                if (std::none_of(s.places.starred_items.begin(), s.places.starred_items.end(),
                                 [&](const app::StarredItem& item) { return item.group == group.id; }))
                    continue;
                ui::FluentMenuItem entry;
                entry.command = kGroupBase + static_cast<int>(group_ids.size());
                entry.text = group.name;
                entry.checked = starred->group == group.id;
                move.children.push_back(std::move(entry));
                group_ids.push_back(group.id);
            }
            if (!move.children.empty()) move.children.back().separator_after = true;
            ui::FluentMenuItem create;
            create.command = kNewGroup;
            create.text = l10n::Get(l10n::StringId::FavoritesGroupNew);
            create.glyph = L"\xE710";
            move.children.push_back(std::move(create));
            if (!starred->group.empty()) {
                ui::FluentMenuItem leave;
                leave.command = kLeaveGroup;
                leave.text = l10n::Get(l10n::StringId::FavoritesGroupLeave);
                move.children.push_back(std::move(leave));
            }
            items.push_back(std::move(move));
        }
        if (starred && starred->group.empty()) {
            // Inside a group a separator would not show; the group has its own.
            ui::FluentMenuItem line;
            line.command = kSeparator;
            line.text = l10n::Get(starred->separator_after ? l10n::StringId::FavoritesSeparatorRemove
                                                           : l10n::StringId::FavoritesSeparatorAdd);
            line.separator_after = true;
            items.push_back(std::move(line));
        }
        if (starred) {
            ui::FluentMenuItem remove;
            remove.command = app::CmdRemoveStarred;
            remove.text = l10n::Get(l10n::StringId::Unstar);
            remove.glyph = L"\xE735";
            items.push_back(std::move(remove));
        }
    }
    const auto* favorite = s.places.FindStarred(path);
    const auto* recent_item = s.places.FindRecent(path);
    const bool known_folder = (favorite && favorite->kind == app::PlaceItemKind::Folder) ||
        (recent_item && recent_item->kind == app::PlaceItemKind::Folder) ||
        (!fs::IsVirtualPath(path) && QuickAccessEntryForPath(s, path));
    if (known_folder) AppendQuickAccessCommand(s, items, {path});
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(items));
    if (HandleQuickAccessCommand(s, cmd, known_folder ? std::vector<std::wstring>{path}
                                                                   : std::vector<std::wstring>{})) return;
    if (cmd >= kGroupBase && cmd < kGroupBase + static_cast<int>(group_ids.size())) {
        s.places.SetStarredItemGroup(path, group_ids[static_cast<size_t>(cmd - kGroupBase)]);
        RefreshStarredViews(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (cmd == kNewGroup) {
        CreateStarredGroupAndEdit(s, path, screen_pt);
        return;
    }
    if (cmd == kLeaveGroup || cmd == kSeparator) {
        if (const app::StarredItem* item = s.places.FindStarred(path)) {
            if (cmd == kLeaveGroup) s.places.SetStarredItemGroup(path, std::wstring{}, false);
            else s.places.SetStarredSeparator(path, !item->separator_after);
        }
        RefreshStarredViews(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (cmd == kOpenNewTab) {
        OpenFolderTab(s, path);
        return;
    }
    if (cmd == app::CmdOpen) {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        const bool is_dir = attrs != INVALID_FILE_ATTRIBUTES &&
                            (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir) NavigateTo(s, path);
        else if (attrs != INVALID_FILE_ATTRIBUTES) {
            s.ops.OpenWith(path);
            RecordRecentOpen(s, path, app::PlaceItemKind::File);
        }
    } else if (cmd == app::CmdEditStarBadge) {
        ShowStarredBadgeEditor(s, path, screen_pt);
    } else if (cmd >= app::CmdTabColorBase &&
               cmd < app::CmdTabColorBase + static_cast<int>(TagColorPalette(s).size())) {
        const uint32_t color = TagColorPalette(s)[static_cast<size_t>(
            cmd - app::CmdTabColorBase)];
        if (auto* starred = s.places.FindStarred(path)) {
            s.places.SetStarredBadge(path, starred->badge, color);
            RefreshStarredViews(s);
        } else if (QuickAccessEntryForPath(s, path)) {
            const app::QuickAccessBadge* quick_badge = s.places.FindQuickAccessBadge(path);
            s.places.SetQuickAccessBadge(path, quick_badge ? quick_badge->badge : std::wstring(), color);
        }
    } else if (cmd == kCustomColor) {
        const app::StarredItem* starred = s.places.FindStarred(path);
        const app::QuickAccessBadge* quick_badge = s.places.FindQuickAccessBadge(path);
        const std::wstring quick_text = quick_badge ? quick_badge->badge : std::wstring();
        uint32_t picked = starred ? starred->badge_rgb
            : quick_badge ? quick_badge->badge_rgb : app::kDefaultBadgeRgb;
        if (ui::ColorPickerPopup::Pick(s.hwnd, &s.compositor, s.menu.get(),
                                       s.scale, screen_pt, picked, s.darkMode, picked)) {
            AppendCustomTagColor(s, picked);
            if (starred) {
                s.places.SetStarredBadge(path, starred->badge, picked);
                RefreshStarredViews(s);
            } else if (QuickAccessEntryForPath(s, path)) {
                s.places.SetQuickAccessBadge(path, quick_text, picked);
            }
        }
    }
    if (cmd == app::CmdRemoveStarred) {
        if (s.places.IsStarred(path)) ToggleStarred(s, path);
    } else if (cmd == app::CmdRemoveRecent) {
        if (s.places.RemoveRecent(path)) RefreshRecentViews(s);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ApplyViewToAllFolders(AppState& s, bool confirm) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return false;
    if (confirm) {
        ui::ConfirmDialogSpec spec;
        spec.title = l10n::Get(l10n::StringId::ApplyViewAllTitle);
        spec.message = l10n::Get(l10n::StringId::ApplyViewAllMessage);
        spec.confirm_text = l10n::Get(l10n::StringId::ApplyViewAllConfirm);
        spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
        if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return false;
        tab = ActiveTab(s);  // the modal dialog pumps messages
        if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return false;
    }
    // Grouping keeps its own per-folder memory (Downloads defaults to Date).
    const int folder_mode = static_cast<int>(tab->EffectiveFolderSort());
    s.appPrefs.folder_views.ApplyToAll(tab->view_mode);
    s.appPrefs.folder_sorts.ApplyToAll({ tab->sort_column, tab->sort_direction });
    // Folders-first goes with the sort: per-folder choices are gone and this
    // folder's becomes the settings default.
    std::vector<std::wstring> resort;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* shown = pane.ActiveTab();
        if (!shown || shown->folder_sort < 0 || fs::IsVirtualPath(shown->current_path)) return;
        if (shown->folder_sort != folder_mode) resort.push_back(shown->current_path);
        shown->folder_sort = -1;
    });
    s.appPrefs.Save();
    if (folder_mode != s.appPrefs.folder_sort_mode) {
        s.settings.FolderSort(folder_mode);  // saves, then re-sorts every visible folder
    } else {
        for (const auto& path : resort) RefreshPath(s, path, RefreshReason::Explicit);
    }
    return true;
}

bool ApplyGroupToAllFolders(AppState& s, int group_by, bool confirm) {
    auto usable = [&s] {
        const app::Tab* tab = ActiveTab(s);
        return tab && !tab->content_results && !tab->current_path.empty() &&
               !fs::IsVirtualPath(tab->current_path);
    };
    if (!usable()) return false;
    const app::GroupBy by = app::GroupByFromInt(group_by);
    if (by == app::GroupBy::Location) return false;  // multi-folder views only
    if (confirm) {
        ui::ConfirmDialogSpec spec;
        spec.title = l10n::Get(l10n::StringId::ApplyViewAllTitle);
        if (by == app::GroupBy::None) {
            spec.message = l10n::Get(l10n::StringId::ApplyGroupNoneMessage);
        } else {
            static constexpr l10n::StringId kLabels[] = {
                l10n::StringId::GroupNone, l10n::StringId::GroupByName, l10n::StringId::GroupByDate,
                l10n::StringId::GroupByType, l10n::StringId::GroupBySize, l10n::StringId::GroupByTag};
            wchar_t buf[512]{};
            swprintf_s(buf, l10n::Get(l10n::StringId::ApplyGroupAllMessageFormat).c_str(),
                       l10n::Get(kLabels[static_cast<int>(by)]).c_str());
            spec.message = buf;
        }
        spec.confirm_text = l10n::Get(l10n::StringId::ApplyViewAllConfirm);
        spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
        if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return false;
        if (!usable()) return false;  // the modal dialog pumps messages
    }
    // Show it here first, then make it the default: ApplyToAll also drops the
    // per-folder entry SetGroupBy just wrote, so this folder follows later changes.
    SetGroupBy(s, static_cast<int>(by));
    s.appPrefs.folder_groups.ApplyToAll(by);
    s.appPrefs.Save();
    return true;
}

// Drop groups with no remaining members (after mass closes / leave operations).
namespace {
// One item keeps its place on screen across a view change: ref (client
// pixels) stays at the same fraction of the item's rect.
struct ZoomAnchor {
    int source = -1;
    bool pointer = false;
    float ref_x = 0.0f, ref_y = 0.0f;
    float frac_x = 0.5f, frac_y = 0.5f;
};

std::optional<ZoomAnchor> CaptureZoomAnchor(AppState& s, const app::Tab& tab, const POINT* point) {
    ui::PaneViewModel pane;
    MaxScrollForActivePane(s, &pane);
    const int count = static_cast<int>(pane.EntryCount());
    // Details tree rows index base entries first, so their sources match the
    // icon views; an expanded child (past the base) simply finds no row later.
    if (count == 0) return std::nullopt;
    const D2D1_RECT_F bounds = FocusedPaneRect(s);
    const D2D1_RECT_F list = s.renderer.PaneListRect(pane, bounds);
    auto on_screen = [&](const D2D1_RECT_F& r) {
        return r.bottom > list.top && r.top < list.bottom && r.right > list.left && r.left < list.right;
    };
    ZoomAnchor a;
    int row = -1;
    if (point) {
        const float x = static_cast<float>(point->x), y = static_cast<float>(point->y);
        if (x >= list.left && x < list.right && y >= list.top && y < list.bottom) {
            a.source = s.renderer.ItemFromPointInPane(pane, bounds, x, y);
            row = pane.ViewIndex(a.source);
            a.pointer = row >= 0;
            a.ref_x = x;
            a.ref_y = y;
        }
    }
    if (row < 0 && tab.selected_index >= 0) {
        const int r = pane.ViewIndex(tab.selected_index);
        if (r >= 0 && r < count && on_screen(s.renderer.ItemRectInPane(pane, bounds, r))) {
            row = r;
            a.source = tab.selected_index;
        }
    }
    if (row < 0) {
        // Nothing to follow at the top of the folder: stay at the top rather
        // than centre a middle item and clip the first row.
        if (tab.scroll_y <= 0.5f && tab.scroll_x <= 0.5f) return std::nullopt;
        const auto [first, last] = s.renderer.VisibleRangeInPane(pane, bounds);
        if (first < 0 || last < first) return std::nullopt;
        row = std::clamp((first + last) / 2, 0, count - 1);
        a.source = pane.SourceIndex(row);
    }
    if (a.source < 0) return std::nullopt;
    const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, bounds, row);
    const float w = item.right - item.left, h = item.bottom - item.top;
    if (w <= 0.0f || h <= 0.0f) return std::nullopt;
    if (a.pointer) {
        a.frac_x = std::clamp((a.ref_x - item.left) / w, 0.0f, 1.0f);
        a.frac_y = std::clamp((a.ref_y - item.top) / h, 0.0f, 1.0f);
    } else {
        a.ref_x = item.left + w * 0.5f;
        a.ref_y = item.top + h * 0.5f;
    }
    return a;
}

// Call with the new view mode set and scroll at 0.
bool RestoreZoomAnchor(AppState& s, app::Tab& tab, const ZoomAnchor& a) {
    ui::PaneViewModel pane;
    MaxScrollForActivePane(s, &pane);
    const int row = pane.ViewIndex(a.source);
    if (row < 0 || row >= static_cast<int>(pane.EntryCount())) return false;
    const D2D1_RECT_F bounds = FocusedPaneRect(s);
    const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, bounds, row);
    tab.scroll_x += item.left + a.frac_x * (item.right - item.left) - a.ref_x;
    tab.scroll_y += item.top + a.frac_y * (item.bottom - item.top) - a.ref_y;
    ClampScroll(s);
    // A focused anchor that grew past the viewport edge is nudged back in;
    // a pointer anchor stays exactly under the pointer.
    if (!a.pointer && a.source == tab.selected_index) EnsureRowVisible(s, tab, a.source);
    s.scrollTargetX = tab.scroll_x;
    s.scrollTargetY = tab.scroll_y;
    return true;
}

// SetViewMode with an icon size (0 = the mode's preset; other views keep the
// tab's size for later). wall: 1 / 0 the header's photo wall / icons button
// (kept for the folder), -1 keeps the tab's.
void ApplyViewMode(AppState& s, ui::ViewMode mode, float icon_dip, const POINT* anchor_point, int wall = -1) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const bool icons = ui::IsZoomIconMode(mode);
    icon_dip = ui::NormalizeIconDip(mode, icon_dip, s.scale);
    const bool to_wall = wall < 0 ? tab->wall : wall != 0;
    bool save = s.appPrefs.folder_views.Set(tab->current_path, mode);
    if (icons) {
        save = s.appPrefs.folder_views.SetIconDip(tab->current_path, icon_dip) || save;
        if (wall >= 0) save = s.appPrefs.folder_views.SetWall(tab->current_path, to_wall) || save;
        save = s.appPrefs.folder_views.SetLastIcon(tab->current_path, mode, icon_dip) || save;
        tab->last_icon_mode = mode;
        tab->last_icon_dip = icon_dip;
    }
    if (save && s.paneSizeDragging) s.paneSizeSavePending = true;  // written on release
    else if (save) s.appPrefs.Save();
    if (tab->view_mode == mode && (!icons || (tab->icon_dip == icon_dip && tab->wall == to_wall))) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    const int old_group = tab->EffectiveGroup();
    const std::optional<ZoomAnchor> anchor = CaptureZoomAnchor(s, *tab, anchor_point);
    tab->view_mode = mode;
    if (icons) {
        tab->icon_dip = icon_dip;
        tab->wall = to_wall;
    }
    // Grouped order applies where groups are drawn (not List, not a photo
    // wall); re-sort when that flips.
    if (tab->EffectiveGroup() != old_group && !fs::IsVirtualPath(tab->current_path))
        RefreshActiveTab(s);
    else if (tab->EffectiveGroup() != old_group && tab->current_path == kNetworkLocationsPath)
        FillNetworkLocationsView(s, *tab);  // grouped order on, or off for List
    ++tab->view_generation;
    tab->scroll_x = 0.0f;
    tab->scroll_y = 0.0f;
    s.scrollTargetY = 0.0f;
    s.scrollAnimating = false;
    // Regrouping re-sorts rows asynchronously, so the anchor would land elsewhere.
    const bool anchored = anchor && tab->EffectiveGroup() == old_group &&
        RestoreZoomAnchor(s, *tab, *anchor);
    if (!anchored && tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
    ClampScroll(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
} // namespace

void SetViewMode(AppState& s, ui::ViewMode mode, const POINT* anchor_point) {
    ApplyViewMode(s, mode, 0.0f, anchor_point);
}

void SetIconSize(AppState& s, float icon_dip, const POINT* anchor_point) {
    ApplyViewMode(s, ui::IconModeForDip(icon_dip, s.scale), icon_dip, anchor_point);
}

void ShowLastIconSize(AppState& s, const POINT* anchor_point) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const bool known = ui::IsZoomIconMode(tab->last_icon_mode);
    ApplyViewMode(s, known ? tab->last_icon_mode : ui::ViewMode::MediumIcons,
                  known ? tab->last_icon_dip : 0.0f, anchor_point);
}

void ShowIconWall(AppState& s, bool wall, const POINT* anchor_point) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const bool known = ui::IsZoomIconMode(tab->last_icon_mode);
    ApplyViewMode(s, known ? tab->last_icon_mode : ui::ViewMode::MediumIcons,
                  known ? tab->last_icon_dip : 0.0f, anchor_point, wall ? 1 : 0);
}

void ChooseViewMode(AppState& s, ui::ViewMode mode) {
    const app::Tab* tab = ActiveTab(s);
    if (tab && tab->wall && ui::IsZoomIconMode(mode)) ApplyViewMode(s, mode, 0.0f, nullptr, 0);
    else SetViewMode(s, mode);
}

void ZoomIconSize(AppState& s, int step, const POINT* anchor_point, bool leave_icons) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || step == 0) return;
    if (!ui::IsZoomIconMode(tab->view_mode)) {
        if (!leave_icons) return;
        if (step > 0) ShowLastIconSize(s, anchor_point);
        else ApplyViewMode(s, ui::ViewMode::Details, 0.0f, anchor_point);
        return;
    }
    const float dip = ui::ZoomIconDip(ui::IconDip(tab->view_mode, tab->icon_dip, s.scale), step, s.scale);
    if (dip > 0.0f) SetIconSize(s, dip, anchor_point);
    else if (leave_icons) ApplyViewMode(s, ui::ViewMode::Details, 0.0f, anchor_point);
}

void ShowViewDropdown(AppState& s, int pane_index) {
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const ui::WindowViewModel vm = BuildVm(s);
    D2D1_RECT_F paneRect = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    if (pane_index >= 0 && pane_index < static_cast<int>(vm.pane_slots.size()))
        paneRect = vm.pane_slots[static_cast<size_t>(pane_index)].rect;
    float filterExpand = 0.0f;
    if (app::Pane* p = PaneAtSlot(s, pane_index)) filterExpand = p->filter_expand;
    else if (s.pane) filterExpand = s.pane->filter_expand;
    const D2D1_RECT_F button = s.renderer.PaneViewButtonRect(paneRect, filterExpand);
    POINT anchor{ static_cast<LONG>(button.left), static_cast<LONG>(button.bottom) };
    ClientToScreen(s.hwnd, &anchor);
    auto items = app::BuildViewMenu(tab->view_mode, s.showDetailsPanel,
        !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path), tab->wall);
    items.back().separator_after=true;
    items.push_back(app::BuildShortcutHints());
    const int cmd = s.menu->TrackPopup(anchor, items);
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// Drum pickers (group_wheel_ui.cpp) instead of popup menus; false restores the menu.
constexpr bool kSortWheel = true;

void ShowSortDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    auto* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    app::BackgroundViewOptions options;
    options.sort_column = tab->sort_column;
    options.sort_direction = tab->sort_direction;
    options.folder_sort = static_cast<int>(tab->EffectiveFolderSort());
    options.can_sort = kind != L"starred" && kind != L"recent";
    options.indexed_search = (kind == L"search" || kind == L"saved-search") && !tab->content_results;
    options.show_path = kind == L"search" || kind == L"saved-search" || kind == L"recycle";
    options.group_virtual = kind == L"search" || kind == L"saved-search" || app::IsTagViewKind(kind) || kind == L"recycle";
    options.can_group = !tab->content_results &&
        ((!tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) || kind == L"recent" ||
         options.group_virtual);
    options.group_by = tab->group_by;
    if constexpr (kSortWheel) {
        if (options.can_sort) {       // not sortable: keep the menu with its disabled rows
            OpenSortWheel(s);
            return;
        }
    }
    const float width = static_cast<float>(s.compositor.Width());
    const float left = s.renderer.EffectiveSidebarWidth(width);
    const auto layout = s.renderer.ToolbarLayoutAt(width,s.renderer.NewButtonWidthPx(width-left < 600*s.scale),
                                                   s.pane ? s.pane->filter_expand : 0.0f);
    POINT anchor{static_cast<LONG>(layout.sort.left),static_cast<LONG>(layout.sort.bottom)};
    ClientToScreen(s.hwnd,&anchor);
    // Grouping has its own toolbar button, so the Sort dropdown is sort-only.
    auto sort_items = app::BuildSortMenu(options);
    const int cmd = s.menu->TrackPopup(anchor,std::move(sort_items));
    if (cmd != app::CmdNone) DispatchMenuCommand(s,cmd);
}

// Drum picker (group_wheel_ui.cpp) instead of the popup menu; false restores the menu.
constexpr bool kGroupWheel = true;

void ShowGroupDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    auto* tab = ActiveTab(s);
    if (!tab) return;
    app::BackgroundViewOptions options;
    options.can_group = s.renderer.ToolbarGroup() >= 0;
    options.group_by = tab->group_by;
    {
        std::wstring kind;
        app::ParsePulsePath(tab->current_path, &kind, nullptr);
        options.group_virtual = kind == L"search" || kind == L"saved-search" || app::IsTagViewKind(kind) || kind == L"recycle";
        options.can_apply_group_all = !tab->content_results && !tab->current_path.empty() &&
                                      !fs::IsVirtualPath(tab->current_path);
    }
    if (!options.can_group) return;
    if constexpr (kGroupWheel) {
        OpenGroupWheel(s);
    } else {
        const float width = static_cast<float>(s.compositor.Width());
        const float left = s.renderer.EffectiveSidebarWidth(width);
        const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale),
                                                       s.pane ? s.pane->filter_expand : 0.0f);
        POINT anchor{static_cast<LONG>(layout.group.left), static_cast<LONG>(layout.group.bottom)};
        ClientToScreen(s.hwnd, &anchor);
        const int cmd = s.menu->TrackPopup(anchor, app::BuildGroupMenu(options).children);
        if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
    }
}

std::vector<ui::SelectionChipAction> SelectionChipActions(AppState& s) {
    // Rebuilt only when the selection or the Pulse menu settings change: the
    // item menu reads tag states and Quick Access targets of the selection.
    struct Cache {
        const AppState* state = nullptr;
        const app::Tab* tab = nullptr;
        std::wstring path;
        std::vector<int> selected;   // the folder items depend on the whole selection
        const void* snapshot = nullptr;  // a drive unlocked or ejected: same rows, new verbs
        uint32_t hidden = 0;
        std::vector<std::wstring> tools;
        std::vector<std::wstring> selection;
        app::BuiltinMenuOrder order;
        std::vector<ui::SelectionChipAction> actions;
    };
    static Cache cache;
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->SelectedCount() == 0) return {};
    std::vector<int> selected = tab->SelectedIndices();
    app::BuiltinMenuOrder order = s.ctxMenuPrefs.BuiltinOrder(app::BuiltinMenuSurface::Item);
    if (cache.state == &s && cache.tab == tab && cache.path == tab->current_path &&
        cache.selected == selected && cache.snapshot == tab->snapshot.get() &&
        cache.hidden == s.ctxMenuPrefs.builtin_hidden && cache.order == order &&
        cache.tools == s.appPrefs.toolbar_tools && cache.selection == s.appPrefs.toolbar_selection)
        return cache.actions;
    cache.state = &s;
    cache.tab = tab;
    cache.path = tab->current_path;
    cache.selected = std::move(selected);
    cache.snapshot = tab->snapshot.get();
    cache.hidden = s.ctxMenuPrefs.builtin_hidden;
    cache.tools = s.appPrefs.toolbar_tools;
    cache.selection = s.appPrefs.toolbar_selection;
    cache.order = std::move(order);
    // Place of each catalog action in the user's list (自定义工具栏 → 选中时); -1: left out.
    std::vector<int> kept(app::kSelectionActionCount, -1);
    for (size_t i = 0; i < cache.selection.size(); ++i) {
        const int at = app::SelectionActionIndex(cache.selection[i]);
        if (at >= 0) kept[static_cast<size_t>(at)] = static_cast<int>(i);
    }
    cache.actions.clear();
    // The chip is its own surface: rows hidden from the right-click menu
    // (Settings > Pulse menu) still belong here, so it reads the full item menu.
    auto items = BuildFinderItemMenu(s, false, L"");
    for (const auto& item : items) {
        if (!item.enabled || item.command == app::CmdNone || !item.children.empty() ||
            !item.quick_swatches.empty() || item.glyph.empty())
            continue;
        const app::BuiltinMenuItem builtin = app::BuiltinItemForCommand(item.command);
        // Selection-only rows the item menu adds on its own (2+ items, two
        // folders, search results) are not in the builtin catalog but belong here.
        // Drive / network / Recycle Bin verbs (AdaptItemMenu) likewise.
        const bool selection_row = item.command == app::CmdBatchRename ||
            item.command == app::CmdGroupIntoFolder || item.command == app::CmdDissolveFolder ||
            item.command == app::CmdCompareSideBySide || item.command == app::CmdOpenPath ||
            item.command == app::CmdUnlockDrive || item.command == app::CmdEjectDrive ||
            item.command == app::CmdDisconnectDrive || item.command == app::CmdMapNetworkDrive ||
            item.command == app::CmdUnpinNetwork || item.command == app::CmdRestoreRecycle ||
            item.command == app::CmdForgetNetworkRecent || item.command == app::CmdRenameNetwork;
        if ((builtin == app::BuiltinMenuItem::Count && !selection_row) ||
            builtin == app::BuiltinMenuItem::Undo || builtin == app::BuiltinMenuItem::SelectCommands)
            continue;
        // Pinning lives in the chip's 固定到… menu.
        if (item.command == app::CmdPinQuickAccess || item.command == app::CmdUnpinQuickAccess ||
            item.command == app::CmdPinWorkspace || item.command == app::CmdPinNetwork)
            continue;
        // Already pinned to the toolbar (自定义工具栏): not twice in one row.
        bool pinned = false;
        for (const auto& id : cache.tools) {
            const int tool = app::ToolbarToolIndex(id);
            pinned = pinned || (tool >= 0 && !kToolbarTools[tool].toggle && kToolbarTools[tool].command == item.command);
        }
        if (pinned) continue;
        const int listed = SelectionActionFor(item.command);
        if (listed >= 0 && kept[static_cast<size_t>(listed)] < 0) continue;
        ui::SelectionChipAction action;
        action.command = item.command;
        action.glyph = item.glyph;
        action.label = item.text;
        action.label = WithoutEllipsis(std::move(action.label));
        if (!item.shortcut.empty()) action.label += L" (" + item.shortcut + L")";
        action.situational = SituationalSelectionCommand(item.command);
        cache.actions.push_back(std::move(action));
    }
    // Split: copy / move to the other pane (new rule, toolbar redesign §5).
    if (LiveLayout(s).VisiblePanes().size() > 1 && !IsRecycleTab(tab) && SelectionCapsOf(s).copy) {
        const struct { int command; l10n::StringId label; const wchar_t* glyph; const wchar_t* key; } kTransfer[] = {
            {app::CmdCopyToTarget, l10n::StringId::CopyToTarget, ui::command_icons::kGlyphCopyToPane, L"Ctrl+Alt+C"},
            {app::CmdMoveToTarget, l10n::StringId::MoveToTarget, ui::command_icons::kGlyphMoveToPane, L"Ctrl+Alt+X"},
        };
        for (const auto& t : kTransfer) {
            ui::SelectionChipAction action;
            action.command = t.command;
            action.glyph = t.glyph;
            action.label = l10n::Get(t.label);
            if (const auto key = app::ShownShortcut(t.key); !key.empty()) action.label += L" (" + key + L")";
            action.situational = true;
            cache.actions.push_back(std::move(action));
        }
    }
    // Drive / network / Recycle Bin verbs first, then the user's order
    // (自定义工具栏 → 选中时); cut / copy / rename / delete already sit on the
    // toolbar. Narrow windows drop actions from the end.
    const auto rank = [&kept](int command) {
        static constexpr int kContext[] = {
            app::CmdUnlockDrive, app::CmdEjectDrive, app::CmdDisconnectDrive, app::CmdRestoreRecycle,
            app::CmdMapNetworkDrive, app::CmdUnpinNetwork, app::CmdRenameNetwork, app::CmdForgetNetworkRecent,
            app::CmdCopyToTarget, app::CmdMoveToTarget, app::CmdCompareSideBySide, app::CmdBatchRename,
            app::CmdOpenPath,
        };
        for (size_t i = 0; i < std::size(kContext); ++i)
            if (kContext[i] == command) return static_cast<int>(i);
        const int listed = SelectionActionFor(command);
        if (listed >= 0) return 100 + kept[static_cast<size_t>(listed)];
        return 10000;
    };
    std::stable_sort(cache.actions.begin(), cache.actions.end(),
                     [&](const ui::SelectionChipAction& a, const ui::SelectionChipAction& b) {
                         return rank(a.command) < rank(b.command);
                     });
    // Up to 2 situational actions, then up to 4 of the user's.
    std::vector<ui::SelectionChipAction> chosen;
    int situational = 0, common = 0;
    for (auto& action : cache.actions) {
        int& n = action.situational ? situational : common;
        if (n >= (action.situational ? ui::kSelChipSituational : ui::kSelChipCommon)) continue;
        ++n;
        chosen.push_back(std::move(action));
    }
    std::stable_partition(chosen.begin(), chosen.end(),
                          [](const ui::SelectionChipAction& a) { return a.situational; });
    cache.actions = std::move(chosen);
    return cache.actions;
}

namespace {
// The count's menu: select more, fewer or alike.
void ShowSelectionChipMenu(AppState& s, POINT anchor) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !EnsureMenu(s)) return;
    constexpr int kKeepFolders = 35101, kKeepFiles = 35102, kSameType = 35103, kClear = 35104;
    constexpr int kCopyNames = 35105;
    const std::vector<int> selected = tab->SelectedIndices();
    const bool listing = tab->snapshot && !tab->content_results;
    int folders = 0, files = 0;
    std::wstring ext;
    bool one_ext = true;
    if (listing) {
        for (int index : selected) {
            if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount()) continue;
            const fs::DirEntry& e = tab->EntryAt(static_cast<size_t>(index));
            if (e.is_dir) { ++folders; one_ext = false; continue; }
            ++files;
            const size_t dot = e.name.rfind(L'.');
            const std::wstring mine = dot == std::wstring::npos || dot == 0 ? std::wstring() : e.name.substr(dot);
            if (files == 1) ext = mine;
            else if (_wcsicmp(ext.c_str(), mine.c_str()) != 0) one_ext = false;
        }
    }
    std::vector<ui::FluentMenuItem> items;
    const auto row = [&items](int command, std::wstring text, const wchar_t* glyph, const wchar_t* key = nullptr) {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = std::move(text);
        item.glyph = glyph;
        if (key) item.shortcut = app::ShownShortcut(key);
        items.push_back(std::move(item));
    };
    row(app::CmdSelectAll, l10n::Get(l10n::StringId::SelectAll), L"\xE8B3", L"Ctrl+A");
    row(app::CmdInvertSelection, l10n::Get(l10n::StringId::InvertSelection), L"\xE8AB");
    row(app::CmdSelectWildcard, l10n::Get(l10n::StringId::SelectWildcard), ui::command_icons::kGlyphSelectWild,
        L"Ctrl+Shift+A");
    items.back().separator_after = true;
    if (folders > 0 && files > 0) {
        row(kKeepFolders, l10n::Get(l10n::StringId::SelKeepFolders), L"\xE8B7");
        row(kKeepFiles, l10n::Get(l10n::StringId::SelKeepFiles), L"\xE8A5");
    }
    if (files > 0 && one_ext && !ext.empty()) {
        std::wstring text = l10n::Get(l10n::StringId::SelSameType);
        if (const size_t at = text.find(L"%s"); at != std::wstring::npos) text.replace(at, 2, ext);
        row(kSameType, std::move(text), L"\xE8A5");
    }
    items.back().separator_after = true;
    if (!selected.empty()) {
        row(kCopyNames, l10n::Get(l10n::StringId::SelCopyNames), L"\xE8C8");
        items.back().separator_after = true;
    }
    row(kClear, l10n::Get(l10n::StringId::ClearSelectionEsc), L"\xE711");
    const int cmd = s.menu->TrackPopup(anchor, std::move(items));
    if (cmd == app::CmdNone) return;
    if (cmd == kCopyNames) {
        // One name a line, in the list's order.
        std::wstring names;
        for (const auto& path : SelectedFullPaths(*tab)) {
            const size_t slash = path.find_last_of(L"\\/");
            if (!names.empty()) names += L"\r\n";
            names += slash == std::wstring::npos ? path : path.substr(slash + 1);
        }
        if (!names.empty()) ops::WriteClipboardText(names);
        return;
    }
    if (cmd == kClear) {
        tab->ClearSelection();
        s.clickCollapseIndex = -1;
    } else if (cmd == kKeepFolders || cmd == kKeepFiles) {
        std::vector<int> keep;
        for (int index : selected)
            if (index >= 0 && static_cast<size_t>(index) < tab->EntryCount() &&
                tab->EntryAt(static_cast<size_t>(index)).is_dir == (cmd == kKeepFolders))
                keep.push_back(index);
        tab->SelectIndices(keep);
    } else if (cmd == kSameType) {
        std::vector<int> same;
        for (size_t i = 0; i < tab->EntryCount(); ++i) {
            const fs::DirEntry& e = tab->EntryAt(i);
            if (e.is_dir || !tab->EntryVisible(static_cast<int>(i)) || e.name.size() <= ext.size()) continue;
            if (_wcsicmp(e.name.c_str() + (e.name.size() - ext.size()), ext.c_str()) == 0) same.push_back(static_cast<int>(i));
        }
        tab->SelectIndices(same);
    } else {
        DispatchMenuCommand(s, cmd);
        return;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// 固定到…: 收藏夹, 快速访问, 工作区, 网络位置 (the last three when the item
// menu offers them for this selection).
void ShowSelectionPinMenu(AppState& s, POINT anchor) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !EnsureMenu(s)) return;
    constexpr int kFavorites = 35110, kToolbarApp = 35111;
    struct Target { std::wstring path; bool dir; };
    std::vector<Target> targets;
    bool all_starred = true;
    for (int index : tab->SelectedIndices()) {
        const std::wstring path = EntryFullPath(*tab, index);
        if (path.empty()) continue;
        const bool dir = index >= 0 && static_cast<size_t>(index) < tab->EntryCount() &&
            tab->EntryAt(static_cast<size_t>(index)).is_dir;
        all_starred = all_starred && s.places.IsStarred(path);
        targets.push_back({path, dir});
    }
    std::vector<ui::FluentMenuItem> items;
    if (!targets.empty()) {
        ui::FluentMenuItem star;
        star.command = kFavorites;
        star.text = l10n::Get(all_starred ? l10n::StringId::Unstar : l10n::StringId::Star);
        star.glyph = all_starred ? L"\xE735" : ui::command_icons::kGlyphPinFavorites;
        items.push_back(std::move(star));
    }
    // One program (or a shortcut, checked when picked) not pinned yet joins
    // the toolbar's apps, like dropping it on the apps' label.
    std::wstring program;
    if (targets.size() == 1 && !targets.front().dir) {
        const std::wstring& path = targets.front().path;
        if ((app::ToolbarFileExt(path) == L".lnk" || ui::IsDropLaunchProgram(path)) &&
            !app::ProgramListHas(s.appPrefs.toolbar_apps, path))
            program = path;
    }
    if (!program.empty()) {
        ui::FluentMenuItem pin;
        pin.command = kToolbarApp;
        pin.text = l10n::Get(l10n::StringId::PinToToolbar);
        pin.glyph = ui::command_icons::kGlyphPinToolbar;
        pin.enabled = s.appPrefs.toolbar_apps.size() < app::kMaxToolbarApps;
        items.push_back(std::move(pin));
    }
    for (const auto& item : BuildFinderItemMenu(s, false, L"")) {
        const wchar_t* glyph = nullptr;
        switch (item.command) {
        case app::CmdPinQuickAccess: case app::CmdUnpinQuickAccess: glyph = ui::command_icons::kGlyphPinQuickAccess; break;
        case app::CmdPinWorkspace: glyph = ui::command_icons::kGlyphPinWorkspace; break;
        case app::CmdPinNetwork: glyph = ui::command_icons::kGlyphPinNetwork; break;
        default: break;
        }
        if (!glyph || !item.enabled) continue;
        ui::FluentMenuItem row;
        row.command = item.command;
        row.text = WithoutEllipsis(item.text);
        row.glyph = glyph;
        row.checked = item.checked;
        items.push_back(std::move(row));
    }
    if (items.empty()) return;
    const int cmd = s.menu->TrackPopup(anchor, std::move(items));
    if (cmd == app::CmdNone) return;
    if (cmd == kFavorites) {
        for (const auto& t : targets)
            if (all_starred || !s.places.IsStarred(t.path))
                ToggleStarred(s, t.path, t.dir ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (cmd == kToolbarApp) {
        const app::ToolbarAppInfo info = app::DescribeToolbarAppNow(program);
        if (!info.exe.empty() && ui::IsDropLaunchProgram(info.exe)) PinToolbarApp(s, program);
        return;
    }
    if (HandleQuickAccessCommand(s, cmd, QuickAccessTargets(tab, false))) return;
    DispatchMenuCommand(s, cmd);
}
} // namespace

void RunToolbarSelectionAction(AppState& s, int action) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->SelectedCount() == 0 || action < 0) return;
    if (action == ui::kSelChipClear) {
        // Same as Esc in the list.
        tab->ClearSelection();
        s.clickCollapseIndex = -1;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    // Menus and pickers open under the clicked chip button.
    const float width = static_cast<float>(s.compositor.Width());
    const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - s.renderer.EffectiveSidebarWidth(width) < 600 * s.scale),
                                                   s.pane ? s.pane->filter_expand : 0.0f);
    const auto actions = SelectionChipActions(s);
    const auto chip = s.renderer.SelectionChipAt(layout, width, static_cast<int>(tab->SelectedCount()),
                                                 static_cast<int>(actions.size()));
    const D2D1_RECT_F button = action == ui::kSelChipLabel ? chip.label
        : chip.buttons[static_cast<size_t>(std::min(action, ui::kSelChipClear))];
    POINT anchor{static_cast<LONG>(button.left), static_cast<LONG>(button.bottom)};
    ClientToScreen(s.hwnd, &anchor);
    if (action == ui::kSelChipLabel) {
        ShowSelectionChipMenu(s, anchor);
        return;
    }
    if (action == ui::kSelChipPin) {
        ShowSelectionPinMenu(s, anchor);
        return;
    }
    if (action == ui::kSelChipMore) {
        ShowItemContextMenu(s, anchor, true);
        return;
    }
    if (action >= static_cast<int>(actions.size())) return;
    const int cmd = actions[static_cast<size_t>(action)].command;
    if (HandleQuickAccessCommand(s, cmd, QuickAccessTargets(tab, false))) return;
    if (cmd == app::CmdTags) ShowTagPicker(s, anchor);
    else DispatchMenuCommand(s, cmd);
}

void ShowToolbarMore(AppState& s) {
    if (!EnsureMenu(s)) return;
    auto* tab=ActiveTab(s);
    if (!tab) return;
    const float width=static_cast<float>(s.compositor.Width());
    const float left=s.renderer.EffectiveSidebarWidth(width);
    // The toolbar as drawn: an open filter folds pins in here too.
    const auto layout=s.renderer.ToolbarLayoutAt(width,s.renderer.NewButtonWidthPx(width-left < 600*s.scale),
                                                 s.pane ? s.pane->filter_expand : 0.0f);
    // The customize button (T5, in place of More): pins without room on the
    // toolbar (all of them in the narrow tier) come first, in a menu that
    // ends in 自定义工具栏…; with none folded it opens the popover directly.
    std::vector<ui::FluentMenuItem> items;
    // Pinned commands without room on the toolbar (all of them when narrow).
    const auto pins=ToolbarPinActions(s);
    const uint64_t separators=app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools);
    size_t pin=0;
    for (const auto& id : s.appPrefs.toolbar_tools) {
        const int tool=app::ToolbarToolIndex(id);
        if (tool<0 || pin>=pins.size()) continue;
        const size_t slot=pin++;
        if (slot<layout.pins.size() && layout.pins[slot].right>layout.pins[slot].left) continue;
        // The user's groups carry over into the menu as separator lines.
        if (!items.empty() && slot<64 && (separators>>slot & 1)) items.back().separator_after=true;
        const ToolbarToolDef& def=kToolbarTools[tool];
        ui::FluentMenuItem item;
        item.command=pins[slot].command; item.text=l10n::Get(def.label);
        item.glyph=def.glyph; item.shortcut=app::ShownShortcut(def.shortcut);
        item.enabled=pins[slot].enabled;
        item.checked=pins[slot].checked;
        item.glyph=pins[slot].glyph;
        if (id==L"split") {   // the split menu itself, as a flyout
            item.command=app::CmdNone;
            item.checked=false;
            item.shortcut.clear();   // a flyout row would show it before the chevron
            item.children=app::BuildSplitMenu(static_cast<int>(LayoutOf(s)));
        }
        items.push_back(std::move(item));
    }
    // Pinned apps without room, after the commands (the toolbar order).
    const auto apps=ToolbarAppPins(s);
    for (size_t i=0;i<apps.size();++i) {
        const size_t slot=pins.size()+i;
        if (slot<layout.pins.size() && layout.pins[slot].right>layout.pins[slot].left) continue;
        ui::FluentMenuItem item;
        item.command=app::CmdToolbarAppBase+static_cast<int>(i);
        item.text=apps[i].name;
        item.icon=apps[i].icon;
        // Like the dimmed toolbar icon: still starts the program (RunToolbarApp
        // then passes none of the selected items).
        items.push_back(std::move(item));
    }
    // 网络位置: the page's own actions lead (pulse_network_design.html §06),
    // so ⋯ is a menu there even when every pin fits.
    if (tab->current_path == kNetworkLocationsPath) AppendNetworkMoreItems(s, items);
    // Everything fits: the button is just 自定义工具栏 (prototype #custBtn).
    if (items.empty()) { ShowToolbarCustomize(s, 0); return; }
    items.back().separator_after=true;
    items.push_back(CustomizeToolbarItem());
    POINT anchor{static_cast<LONG>(layout.overflow.left),static_cast<LONG>(layout.overflow.bottom)};
    ClientToScreen(s.hwnd,&anchor);
    const int cmd=s.menu->TrackPopup(anchor,items);
    if (RunToolbarAppMenuCommand(s,cmd)) return;
    if (cmd!=app::CmdNone) DispatchMenuCommand(s,cmd);
}

// Right-click on a pinned tool: group it with a separator, take it off, or
// open 自定义工具栏.
void ShowToolbarPinMenu(AppState& s, int slot, POINT screen) {
    if (!EnsureMenu(s) || slot < 0) return;
    const auto only = app::ToolbarToolsOnly(s.appPrefs.toolbar_tools);
    if (static_cast<size_t>(slot) >= only.size()) { ShowToolbarCustomize(s, 0); return; }
    constexpr int kAddSeparator = 35001, kRemoveSeparator = 35002, kRemoveTool = 35003;
    const size_t next = static_cast<size_t>(slot) + 1;
    const bool has_after = next < 64 && (app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools) >> next & 1);
    std::vector<ui::FluentMenuItem> items;
    if (next < only.size()) {
        ui::FluentMenuItem sep;
        sep.command = has_after ? kRemoveSeparator : kAddSeparator;
        sep.text = l10n::Get(has_after ? l10n::StringId::ToolbarRemoveSeparator : l10n::StringId::ToolbarAddSeparator);
        sep.glyph = L"\xE76F";   // a vertical bar between two groups
        items.push_back(std::move(sep));
    }
    ui::FluentMenuItem remove;
    remove.command = kRemoveTool;
    remove.text = l10n::Get(l10n::StringId::ToolbarRemoveTool);
    remove.glyph = L"\xE711";
    remove.separator_after = true;
    items.push_back(std::move(remove));
    items.push_back(CustomizeToolbarItem());
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    auto& tools = s.appPrefs.toolbar_tools;
    std::vector<std::wstring> changed = tools;
    if (cmd == kAddSeparator || cmd == kRemoveSeparator)
        changed = app::SetToolbarSeparatorAfter(tools, static_cast<size_t>(slot), cmd == kAddSeparator);
    else if (cmd == kRemoveTool)
        changed = app::RemoveToolbarTool(tools, static_cast<size_t>(slot));
    else if (cmd != app::CmdNone) { DispatchMenuCommand(s, cmd); return; }
    if (changed == tools) return;
    tools = std::move(changed);
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowToolbarAppPinMenu(AppState& s, size_t app, POINT screen) {
    auto& apps = s.appPrefs.toolbar_apps;
    if (!EnsureMenu(s) || app >= apps.size()) return;
    constexpr int kAddSeparator = 35001, kRemoveSeparator = 35002, kUnpin = 35003;
    auto& breaks = s.appPrefs.toolbar_app_breaks;
    const std::wstring path = apps[app];
    const bool has_after = app::ToolbarAppBreakAfter(breaks, path);
    std::vector<ui::FluentMenuItem> items;
    if (app + 1 < std::min<size_t>(apps.size(), app::kMaxToolbarApps)) {
        ui::FluentMenuItem sep;
        sep.command = has_after ? kRemoveSeparator : kAddSeparator;
        sep.text = l10n::Get(has_after ? l10n::StringId::ToolbarRemoveSeparator : l10n::StringId::ToolbarAddSeparator);
        sep.glyph = L"\xE76F";
        items.push_back(std::move(sep));
    }
    ui::FluentMenuItem remove;
    remove.command = kUnpin;
    remove.text = l10n::Get(l10n::StringId::ToolbarRemoveTool);
    remove.glyph = L"\xE711";
    remove.separator_after = true;
    items.push_back(std::move(remove));
    items.push_back(CustomizeToolbarItem());
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    const auto drop_break = [&] {
        breaks.erase(std::remove_if(breaks.begin(), breaks.end(), [&](const std::wstring& b) {
            return _wcsicmp(b.c_str(), path.c_str()) == 0;
        }), breaks.end());
    };
    if (cmd == kAddSeparator) breaks.push_back(path);
    else if (cmd == kRemoveSeparator) drop_break();
    else if (cmd == kUnpin) { drop_break(); apps.erase(apps.begin() + static_cast<std::ptrdiff_t>(app)); }
    else { if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd); return; }
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Toolbar picker T3c: the tools tile hovered in the open picker (-1 none),
// shown as a dashed preview pin after the pinned ones; and a toolbar pin
// being dragged while the picker is open.
namespace {
int g_toolbar_preview = -1;
// The picker's hovered pinned tile (T3d): tools page tool index, or an app path.
int g_toolbar_link_tool = -1;
std::wstring g_toolbar_link_app;
struct ToolbarPinDrag {
    bool down = false, active = false, out = false, moved = false;
    int index = -1;        // the pin's slot now (tools, then apps)
    POINT start{};
    float grab = 0.0f;     // pointer x - the pin's left edge at the press
    float left = 0.0f, dy = 0.0f;
};
ToolbarPinDrag g_pin_drag;

void PushToolbarPinDrag(AppState& s) {
    ui::ToolbarPinDragState state;
    if (g_pin_drag.active) {
        state.index = g_pin_drag.index;
        state.left = g_pin_drag.left;
        state.dy = g_pin_drag.dy;
        state.out = g_pin_drag.out;
    }
    if (g_toolbar_preview >= 0) state.preview_command = ToolbarToolCommand(static_cast<size_t>(g_toolbar_preview));
    // The linked pin's slot, worked out now: the toolbar order may have moved.
    if (g_toolbar_link_tool >= 0 && static_cast<size_t>(g_toolbar_link_tool) < app::kToolbarToolCount) {
        int slot = 0;
        for (const auto& id : s.appPrefs.toolbar_tools) {
            if (app::ToolbarToolIndex(id) < 0) continue;
            if (id == app::kToolbarToolIds[g_toolbar_link_tool]) { state.linked = slot; break; }
            ++slot;
        }
    } else if (!g_toolbar_link_app.empty()) {
        const auto& apps = s.appPrefs.toolbar_apps;
        for (size_t j = 0; j < apps.size() && j < app::kMaxToolbarApps; ++j)
            if (_wcsicmp(apps[j].c_str(), g_toolbar_link_app.c_str()) == 0) {
                state.linked = static_cast<int>(ToolbarPinActions(s).size() + j);
                break;
            }
    }
    ui::SetToolbarPinDrag(&s.renderer, state);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}

std::vector<ui::SelectionChipAction> ToolbarPinActions(AppState& s) {
    std::vector<ui::SelectionChipAction> out;
    const app::Tab* tab = ActiveTab(s);
    if ((s.appPrefs.toolbar_tools.empty() && g_toolbar_preview < 0) || !tab) return out;
    const bool has_selection = tab->SelectedCount() > 0;
    app::SelectionCaps caps{};
    bool caps_known = false;
    std::vector<std::wstring> ids = s.appPrefs.toolbar_tools;
    if (g_toolbar_preview >= 0 && static_cast<size_t>(g_toolbar_preview) < app::kToolbarToolCount &&
        !ToolbarToolPinned(s, app::kToolbarToolIds[g_toolbar_preview]))
        ids.emplace_back(app::kToolbarToolIds[g_toolbar_preview]);
    for (const auto& id : ids) {
        const int tool = app::ToolbarToolIndex(id);
        if (tool < 0 || out.size() >= static_cast<size_t>(ui::kToolbarCommandPins)) continue;
        const ToolbarToolDef& def = kToolbarTools[tool];
        ui::SelectionChipAction action;
        action.command = ToolbarToolCommand(static_cast<size_t>(tool));
        action.glyph = def.glyph;
        action.label = l10n::Get(def.label);
        if (const auto key = app::ShownShortcut(def.shortcut); !key.empty()) action.label += L" (" + key + L")";
        if (def.toggle) {
            action.checked = ToolbarToggleOn(s, static_cast<size_t>(tool));
            out.push_back(std::move(action));
            continue;
        }
        switch (def.command) {
        case app::CmdNewTextFile: case app::CmdNewMarkdownFile: action.enabled = CanCreateHere(tab); break;
        case app::CmdCopyPath: action.enabled = has_selection || !tab->current_path.empty(); break;
        case app::CmdOpenTerminal: action.enabled = !TerminalTarget(s).empty(); break;
        case app::CmdProperties: action.enabled = has_selection; break;
        case app::CmdCompressZip: action.enabled = CanCompressSelection(s); break;
        case app::CmdUndo: action.enabled = s.ops.CanUndo(); break;
        // Same rules as the former fixed buttons: what is selected decides
        // (a drive takes a rename only); paste needs files or a tray batch
        // and a folder that takes them. Worked out once, when asked for.
        case app::CmdCut: case app::CmdCopy: case app::CmdRename: case app::CmdDelete: {
            if (!caps_known) { caps = SelectionCapsOf(s); caps_known = true; }
            action.enabled = def.command == app::CmdCut ? caps.cut : def.command == app::CmdCopy ? caps.copy
                           : def.command == app::CmdRename ? caps.rename : caps.del;
            break;
        }
        case app::CmdPaste:
            action.enabled = CanCreateHere(tab) ? (!s.tray.batches().empty() || ClipboardHasFiles())
                                                : CanPasteHere(tab) && ClipboardHasFiles();
            break;
        case app::CmdSelectAll: case app::CmdInvertSelection: case app::CmdSelectWildcard:
            action.enabled = tab->snapshot && tab->EntryCount() > 0;
            break;
        case 0:   // split: lit while the window is split
            action.checked = LiveLayout(s).VisiblePanes().size() > 1;
            break;
        default: break;
        }
        out.push_back(std::move(action));
    }
    return out;
}

void RunToolbarAction(AppState& s, bool pin, int index) {
    if (!pin) {
        RunToolbarSelectionAction(s, index);
        return;
    }
    const auto pins = ToolbarPinActions(s);
    if (index >= static_cast<int>(pins.size())) {   // pinned apps follow the commands
        RunToolbarApp(s, static_cast<size_t>(index - static_cast<int>(pins.size())));
        return;
    }
    if (index < 0) return;
    const ui::SelectionChipAction& action = pins[static_cast<size_t>(index)];
    if (action.enabled) DispatchMenuCommand(s, action.command);
}

bool ToolbarNewCreatesFolder(const AppState& s) {
    return !(s.appPrefs.toolbar_new_primary == 1 && s.appPrefs.toolbar_last_new == 1);
}

// Screen rect of the toolbar's More button (the picker hangs from it).
static RECT ToolbarMoreScreenRect(AppState& s) {
    const float width = static_cast<float>(s.compositor.Width());
    const float left = s.renderer.EffectiveSidebarWidth(width);
    const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale),
                                                   s.pane ? s.pane->filter_expand : 0.0f);
    POINT a{static_cast<LONG>(layout.overflow.left), static_cast<LONG>(layout.overflow.top)};
    POINT b{static_cast<LONG>(layout.overflow.right), static_cast<LONG>(layout.overflow.bottom)};
    if (layout.overflow.right <= layout.overflow.left) {   // no More button: the toolbar's right end
        const auto bar = s.renderer.ToolbarRect(width);
        a = {static_cast<LONG>(bar.right - 40 * s.scale), static_cast<LONG>(bar.top)};
        b = {static_cast<LONG>(bar.right - 8 * s.scale), static_cast<LONG>(bar.bottom)};
    }
    ClientToScreen(s.hwnd, &a);
    ClientToScreen(s.hwnd, &b);
    return RECT{a.x, a.y, b.x, b.y};
}

// 自定义工具栏 → 排序与筛选: the toolbar's view buttons (not pins; they
// show or hide in place, AppPrefs::toolbar_show_*).
struct ToolbarViewDef {
    std::wstring_view id;
    const wchar_t* glyph;
    l10n::StringId label;
};
static constexpr ToolbarViewDef kToolbarViews[] = {
    {L"sort", L"\xE8CB", l10n::StringId::ToolbarSort},
    {L"filter", L"\xE71C", l10n::StringId::ToolbarFilter},
    {L"group", L"\xF168", l10n::StringId::ToolbarGroup},
    {L"fav_bar", L"\xE728", l10n::StringId::FavoritesBar},   // 收藏栏, on the 窗格 section
};
static constexpr size_t kToolbarViewCount = std::size(kToolbarViews);
static bool& ToolbarViewPref(AppState& s, size_t i) {
    return i == 0 ? s.appPrefs.toolbar_show_sort : i == 1 ? s.appPrefs.toolbar_show_filter
         : i == 2 ? s.appPrefs.toolbar_show_group : s.appPrefs.favorites_bar;
}
static bool ToolbarViewShown(const AppState& s, size_t i) {
    return i == 0 ? s.appPrefs.toolbar_show_sort : i == 1 ? s.appPrefs.toolbar_show_filter
         : i == 2 ? s.appPrefs.toolbar_show_group : s.appPrefs.favorites_bar;
}

// Section of each toolbar_tools.h id on the picker's tools page.
// Sections follow the v3 prototype: 文件 / 选择 / 显示 / 窗格 / 更多.
static int ToolbarToolSection(std::wstring_view id) {
    for (std::wstring_view file : {L"new_text", L"copy_path", L"properties", L"undo", L"cut", L"copy",
                                   L"paste", L"rename", L"delete", L"new_md", L"zip"})
        if (id == file) return 0;
    if (id == L"select_all" || id == L"invert" || id == L"select_wild") return 1;
    for (std::wstring_view pane : {L"sidebar", L"split", L"details", L"columns", L"apply_all", L"recent"})
        if (id == pane) return 3;
    for (std::wstring_view more : {L"terminal", L"search_all", L"adv_search", L"recycle", L"map_drive",
                                   L"shortcuts", L"settings"})
        if (id == more) return 4;
    return 2;   // the display switches
}

// The tools page order: pinned tools in toolbar order lead; the rest follow
// in the prototype's catalog order.
static std::vector<size_t> ToolbarPinsOrder(const AppState& s) {
    static constexpr std::wstring_view kCatalogOrder[] = {
        L"cut", L"copy", L"paste", L"rename", L"delete", L"undo", L"new_text", L"new_md", L"copy_path",
        L"properties", L"zip", L"select_all", L"invert", L"select_wild", L"hidden", L"protected", L"extensions", L"folder_size", L"compact",
        L"zebra", L"size_bar", L"smart_date", L"outline", L"theme", L"split", L"details", L"columns",
        L"sidebar", L"apply_all", L"recent", L"terminal", L"search_all", L"adv_search", L"recycle",
        L"map_drive", L"shortcuts", L"settings"};
    static_assert(std::size(kCatalogOrder) == app::kToolbarToolCount, "every tool has a place");
    std::vector<size_t> order;
    std::vector<char> placed(app::kToolbarToolCount, 0);
    for (const auto& id : s.appPrefs.toolbar_tools) {
        const int at = app::ToolbarToolIndex(id);
        if (at >= 0 && !placed[at]) { placed[at] = 1; order.push_back(static_cast<size_t>(at)); }
    }
    for (std::wstring_view id : kCatalogOrder) {
        const int at = app::ToolbarToolIndex(id);
        if (at >= 0 && !placed[at]) { placed[at] = 1; order.push_back(static_cast<size_t>(at)); }
    }
    for (size_t i = 0; i < kToolbarViewCount; ++i) order.push_back(app::kToolbarToolCount + i);
    return order;
}

void ShowToolbarCustomize(AppState& s, int page) {
    if (ui::ToolbarPickerOpen()) return;
    // The button's own tooltip would otherwise hang over the picker.
    s.tooltipText.clear();
    s.hoverSince = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    ui::ToolbarCustomizeSpec spec;
    spec.title = l10n::Get(l10n::StringId::CustomizeToolbar);
    spec.done_text = l10n::Get(l10n::StringId::ToolbarDone);
    spec.tools_tab = l10n::Get(l10n::StringId::ToolbarPickerToolsTab);
    spec.apps_tab = l10n::Get(l10n::StringId::ToolbarPickerAppsTab);
    spec.sel_tab = l10n::Get(l10n::StringId::ToolbarPickerSelTab);
    spec.sel_desc = l10n::Get(l10n::StringId::ToolbarPickerSelDesc);
    spec.on_toolbar_text = l10n::Get(l10n::StringId::ToolbarPickerOnToolbar);
    spec.tools_full_text = l10n::Get(l10n::StringId::ToolbarPickerToolsFull);
    spec.max_tools = app::kMaxToolbarTools + kToolbarViewCount;
    spec.keys_text = l10n::Get(l10n::StringId::KeyboardShortcuts);
    spec.keys_hint = app::ShownShortcut(L"Ctrl+/");
    spec.on_shortcuts = [&s] { DispatchMenuCommand(s, app::CmdShortcutHelp); };
    spec.tools_desc = l10n::Get(l10n::StringId::ToolbarPickerToolsDesc);
    spec.apps_desc = l10n::Get(l10n::StringId::ToolbarPickerAppsDesc);
    spec.search_hint = l10n::Get(l10n::StringId::ToolbarPickerSearch);
    spec.empty_text = l10n::Get(l10n::StringId::ToolbarPickerEmpty);
    spec.apps_full_text = l10n::Get(l10n::StringId::ToolbarPickerAppsFull);
    spec.sections = {l10n::Get(l10n::StringId::ToolbarSectionFile), l10n::Get(l10n::StringId::ToolbarSectionSelect),
                     l10n::Get(l10n::StringId::ToolbarSectionShow), l10n::Get(l10n::StringId::ToolbarSectionPane),
                     l10n::Get(l10n::StringId::ToolbarSectionMore), l10n::Get(l10n::StringId::ToolbarSectionView)};
    for (size_t i = 0; i < std::size(kToolbarTools); ++i) {
        ui::ToolbarPickerTile tile;
        tile.glyph = kToolbarTools[i].glyph;
        tile.label = l10n::Get(kToolbarTools[i].label);
        tile.keywords = std::wstring(app::kToolbarToolIds[i]) + L" " + app::ShownShortcut(kToolbarTools[i].shortcut);
        tile.section = ToolbarToolSection(app::kToolbarToolIds[i]);
        tile.checked = ToolbarToolPinned(s, app::kToolbarToolIds[i]);
        tile.toggle = kToolbarTools[i].toggle;
        tile.toggle_on = tile.toggle && ToolbarToggleOn(s, i);
        spec.pins.push_back(std::move(tile));
    }
    // 排序与筛选: the view buttons at the toolbar's right, shown or hidden
    // in place (tiles kToolbarToolCount + i).
    for (size_t i = 0; i < kToolbarViewCount; ++i) {
        ui::ToolbarPickerTile tile;
        tile.glyph = kToolbarViews[i].glyph;
        tile.label = l10n::Get(kToolbarViews[i].label);
        tile.keywords = std::wstring(kToolbarViews[i].id);
        if (kToolbarViews[i].id == L"fav_bar") tile.keywords += L" " + app::ShownShortcut(L"Ctrl+Shift+B");
        tile.section = kToolbarViews[i].id == L"fav_bar" ? 3 : 5;
        tile.checked = ToolbarViewShown(s, i);
        spec.pins.push_back(std::move(tile));
    }
    spec.pins_order = ToolbarPinsOrder(s);

    // 选中时: the chip's actions, kept ones in chip order first.
    std::vector<bool> kept(app::kSelectionActionCount, false);
    for (const auto& id : s.appPrefs.toolbar_selection) {
        const int at = app::SelectionActionIndex(id);
        if (at < 0) continue;
        kept[static_cast<size_t>(at)] = true;
        spec.selection_order.push_back(static_cast<size_t>(at));
    }
    for (size_t i = 0; i < std::size(kSelectionActions); ++i) {
        const SelectionActionDef& def = kSelectionActions[i];
        ui::ToolbarPickerTile tile;
        tile.glyph = def.glyph;
        tile.label = WithoutEllipsis(l10n::Get(def.label));
        tile.keywords = std::wstring(app::kSelectionActionIds[i]);
        switch (def.hint) {
        case kSelHintMulti: tile.badge = l10n::Get(l10n::StringId::ToolbarSelHintMulti); break;
        case kSelHintTwoFolders: tile.badge = l10n::Get(l10n::StringId::ToolbarSelHintTwoFolders); break;
        case kSelHintFolders: tile.badge = l10n::Get(l10n::StringId::ToolbarSelHintFolders); break;
        case kSelHintSearch: tile.badge = l10n::Get(l10n::StringId::ToolbarSelHintSearch); break;
        default: break;
        }
        tile.tool = def.tool ? app::ToolbarToolIndex(def.tool) : -1;
        tile.checked = kept[i];
        // Pinning moved to the chip's 固定到… menu: older prefs keep their
        // pin tiles, nobody is offered new ones.
        const std::wstring_view id = app::kSelectionActionIds[i];
        tile.hidden = !kept[i] && (id == L"quick_access" || id == L"pin_workspace" || id == L"pin_network");
        spec.selection.push_back(std::move(tile));
        if (!kept[i]) spec.selection_order.push_back(i);
    }
    spec.apps_pinned = s.appPrefs.toolbar_apps.size();
    spec.load_apps = [&s](ui::ToolbarCustomizeSpec& later) {
        FillToolbarAppRows(s, later);
    };
    spec.page = page;
    spec.anchor = ToolbarMoreScreenRect(s);
    const auto save = [&s] {
        if (!s.isolatedTest) s.appPrefs.Save();
        InvalidateRect(s.hwnd, nullptr, FALSE);
    };
    // The picker hands over the checked tiles in its order: the toolbar's.
    spec.on_tools = [&s, save](const std::vector<size_t>& pinned) {
        std::vector<std::wstring> tools;
        bool view[kToolbarViewCount] = {};
        for (size_t index : pinned) {
            if (index < app::kToolbarToolCount) tools.emplace_back(app::kToolbarToolIds[index]);
            else if (index - app::kToolbarToolCount < kToolbarViewCount) view[index - app::kToolbarToolCount] = true;
        }
        bool changed = false;
        for (size_t i = 0; i < kToolbarViewCount; ++i)
            if (ToolbarViewShown(s, i) != view[i]) { ToolbarViewPref(s, i) = view[i]; changed = true; }
        tools = app::KeepToolbarSeparators(s.appPrefs.toolbar_tools, tools);
        if (tools != s.appPrefs.toolbar_tools) {
            s.appPrefs.toolbar_tools = std::move(tools);
            if (g_toolbar_link_tool >= 0) PushToolbarPinDrag(s);   // the ring follows its pin
            changed = true;
        }
        if (changed) save();
    };
    spec.on_selection = [&s, save](const std::vector<size_t>& shown) {
        std::vector<std::wstring> actions;
        for (size_t index : shown)
            if (index < app::kSelectionActionCount) actions.emplace_back(app::kSelectionActionIds[index]);
        if (actions == s.appPrefs.toolbar_selection) return;
        s.appPrefs.toolbar_selection = std::move(actions);
        save();
    };
    spec.on_switch = [&s](size_t index) {
        FlipToolbarTool(s, index);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return ToolbarToggleOn(s, index);
    };
    spec.on_apps = [&s, save](const std::vector<ui::ToolbarPickerTile>& apps) {
        if (ApplyToolbarAppRows(s, apps)) {
            save();
            if (!g_toolbar_link_app.empty()) PushToolbarPinDrag(s);
        }
    };
    // T3c: the toolbar beside the open picker.
    spec.on_preview = [&s](int index) {
        // The view tiles show or hide in place: no dashed preview pin.
        g_toolbar_preview = index >= 0 && static_cast<size_t>(index) < app::kToolbarToolCount ? index : -1;
        PushToolbarPinDrag(s);
    };
    // T3d: a pinned tile hovered in the picker rings its toolbar pin (the
    // selection page's chip is not on the toolbar, so it rings nothing).
    spec.on_link = [&s](int page, int index, const std::wstring& value) {
        g_toolbar_link_tool = page == 0 && index >= 0 && static_cast<size_t>(index) < app::kToolbarToolCount ? index : -1;
        g_toolbar_link_app = page == 2 ? value : std::wstring{};
        PushToolbarPinDrag(s);
    };
    spec.keep_open = [&s](POINT screen) {
        ScreenToClient(s.hwnd, &screen);
        const ui::WindowViewModel vm = BuildVm(s, false);
        const D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
        return s.renderer.HitTest(vm, rect, (float)screen.x, (float)screen.y).region == ui::HitTestResult::ToolbarPin;
    };
    spec.resync = [&s](ui::ToolbarCustomizeSpec& now) {
        now.pins_order = ToolbarPinsOrder(s);
        for (size_t i = 0; i < now.pins.size() && i < app::kToolbarToolCount; ++i) {
            now.pins[i].checked = ToolbarToolPinned(s, app::kToolbarToolIds[i]);
            now.pins[i].toggle_on = now.pins[i].toggle && ToolbarToggleOn(s, i);
        }
        for (size_t i = 0; i < kToolbarViewCount && app::kToolbarToolCount + i < now.pins.size(); ++i)
            now.pins[app::kToolbarToolCount + i].checked = ToolbarViewShown(s, i);
        now.apps_pinned = s.appPrefs.toolbar_apps.size();
        std::vector<char> placed(now.apps.size(), 0);
        for (const auto& path : s.appPrefs.toolbar_apps)
            for (size_t i = 0; i < now.apps.size(); ++i)
                if (!placed[i] && _wcsicmp(now.apps[i].value.c_str(), path.c_str()) == 0) {
                    placed[i] = 1;
                    now.apps_order.push_back(i);
                    break;
                }
        for (size_t i = 0; i < now.apps.size(); ++i) {
            now.apps[i].checked = placed[i] != 0;
            if (!placed[i]) now.apps_order.push_back(i);
        }
    };
    // Separators: tools by slot among the pinned tools, apps by path.
    spec.add_break_text = l10n::Get(l10n::StringId::ToolbarAddSeparator);
    spec.remove_break_text = l10n::Get(l10n::StringId::ToolbarRemoveSeparator);
    const auto tool_slot = [&s](size_t index) -> int {
        if (index >= app::kToolbarToolCount) return -1;
        const auto only = app::ToolbarToolsOnly(s.appPrefs.toolbar_tools);
        for (size_t k = 0; k + 1 < only.size(); ++k)
            if (only[k] == app::kToolbarToolIds[index]) return static_cast<int>(k);
        return -1;   // not pinned, or the last tool
    };
    const auto app_slot = [&s](const std::wstring& value) -> int {
        const auto& apps = s.appPrefs.toolbar_apps;
        const size_t n = std::min<size_t>(apps.size(), app::kMaxToolbarApps);
        for (size_t k = 0; k + 1 < n; ++k)
            if (_wcsicmp(apps[k].c_str(), value.c_str()) == 0) return static_cast<int>(k);
        return -1;
    };
    spec.break_state = [&s, tool_slot, app_slot](int page, size_t index, const std::wstring& value) -> int {
        if (page == 0) {
            const int slot = tool_slot(index);
            if (slot < 0 || slot + 1 >= 64) return -1;
            return (app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools) >> (slot + 1) & 1) ? 1 : 0;
        }
        if (page == 2 && app_slot(value) >= 0)
            return app::ToolbarAppBreakAfter(s.appPrefs.toolbar_app_breaks, value) ? 1 : 0;
        return -1;
    };
    spec.toggle_break = [&s, save, tool_slot, app_slot](int page, size_t index, const std::wstring& value) {
        if (page == 0) {
            const int slot = tool_slot(index);
            if (slot < 0) return;
            const bool on = !(app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools) >> (slot + 1) & 1);
            s.appPrefs.toolbar_tools = app::SetToolbarSeparatorAfter(s.appPrefs.toolbar_tools, static_cast<size_t>(slot), on);
        } else if (page == 2 && app_slot(value) >= 0) {
            auto& breaks = s.appPrefs.toolbar_app_breaks;
            if (app::ToolbarAppBreakAfter(breaks, value)) {
                breaks.erase(std::remove_if(breaks.begin(), breaks.end(), [&](const std::wstring& b) {
                    return _wcsicmp(b.c_str(), value.c_str()) == 0;
                }), breaks.end());
            } else {
                breaks.push_back(value);
            }
        } else {
            return;
        }
        save();
    };
    spec.dark_now = [&s] { return s.darkMode; };
    ui::ShowToolbarPicker(s.hwnd, spec, s.darkMode, s.accentColor);
    g_toolbar_preview = -1;
    g_toolbar_link_tool = -1;
    g_toolbar_link_app.clear();
    PushToolbarPinDrag(s);
}

// ---- T3c: dragging a toolbar pin while the picker is open -------------------
// Along the row it reorders (tools among tools, apps among apps; the others
// slide out of the way); let go more than 34 DIP below the row it leaves the
// toolbar. A press without a drag is a click. Either way the picker then
// rereads the toolbar and takes the activation back.
static ui::ToolbarLayout CurrentToolbarLayout(AppState& s) {
    const float width = static_cast<float>(s.compositor.Width());
    const float left = s.renderer.EffectiveSidebarWidth(width);
    return s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale),
                                      s.pane ? s.pane->filter_expand : 0.0f);
}

bool ToolbarPinDragging() { return g_pin_drag.down; }

void BeginToolbarPinDrag(AppState& s, int index, POINT pt) {
    const auto layout = CurrentToolbarLayout(s);
    if (index < 0 || index >= static_cast<int>(layout.pins.size())) return;
    g_pin_drag = ToolbarPinDrag{};
    g_pin_drag.down = true;
    g_pin_drag.index = index;
    g_pin_drag.start = pt;
    g_pin_drag.grab = static_cast<float>(pt.x) - layout.pins[static_cast<size_t>(index)].left;
    SetCapture(s.hwnd);
}

void MoveToolbarPinDrag(AppState& s, POINT pt) {
    ToolbarPinDrag& d = g_pin_drag;
    if (!d.down) return;
    const float dx = static_cast<float>(pt.x - d.start.x), dy = static_cast<float>(pt.y - d.start.y);
    const int tools = static_cast<int>(std::min<size_t>(app::ToolbarToolsOnly(s.appPrefs.toolbar_tools).size(),
                                                        ui::kToolbarCommandPins));
    const int pins = static_cast<int>(ToolbarPinActions(s).size());
    const int apps = static_cast<int>(std::min<size_t>(s.appPrefs.toolbar_apps.size(), app::kMaxToolbarApps));
    const bool is_tool = d.index < tools;
    const bool is_app = d.index >= pins && d.index < pins + apps;
    if (!is_tool && !is_app) return;   // the preview pin
    if (!d.active) {
        if (std::hypot(dx, dy) < 4.0f * s.scale) return;
        d.active = true;
    }
    const auto layout = CurrentToolbarLayout(s);
    const D2D1_RECT_F home = layout.pins[static_cast<size_t>(d.index)];
    const float w = home.right - home.left;
    d.left = static_cast<float>(pt.x) - d.grab;
    // Below the row it stays in sight (14 DIP down at most), faded once
    // letting go would take it off.
    d.dy = std::clamp(dy, -4.0f * s.scale, 14.0f * s.scale);
    d.out = dy > 34.0f * s.scale;
    // The slot whose centre is nearest the pin's, in its own group.
    const int from = is_tool ? 0 : pins, to = is_tool ? tools : pins + apps;
    int best = d.index;
    float best_dist = 1e9f;
    for (int i = from; i < to && i < static_cast<int>(layout.pins.size()); ++i) {
        const D2D1_RECT_F r = layout.pins[static_cast<size_t>(i)];
        if (r.right <= r.left) continue;
        const float dist = std::fabs((r.left + r.right) * 0.5f - (d.left + w * 0.5f));
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    if (best != d.index && !d.out) {
        auto move = [](std::vector<std::wstring>& list, int a, int b) {
            if (a < 0 || b < 0 || a >= static_cast<int>(list.size()) || b >= static_cast<int>(list.size())) return;
            std::wstring item = std::move(list[static_cast<size_t>(a)]);
            list.erase(list.begin() + a);
            list.insert(list.begin() + b, std::move(item));
        };
        if (is_tool) s.appPrefs.toolbar_tools = app::MoveToolbarTool(s.appPrefs.toolbar_tools,
                                                                     static_cast<size_t>(d.index), static_cast<size_t>(best));
        else move(s.appPrefs.toolbar_apps, d.index - pins, best - pins);
        d.index = best;
        d.moved = true;
    }
    PushToolbarPinDrag(s);
}

void EndToolbarPinDrag(AppState& s, bool cancel) {
    ToolbarPinDrag d = g_pin_drag;
    if (!d.down) return;
    g_pin_drag = ToolbarPinDrag{};
    if (GetCapture() == s.hwnd) ReleaseCapture();
    bool changed = d.moved;
    if (d.active && d.out && !cancel) {
        const int tools = static_cast<int>(std::min<size_t>(app::ToolbarToolsOnly(s.appPrefs.toolbar_tools).size(),
                                                            ui::kToolbarCommandPins));
        const int pins = static_cast<int>(ToolbarPinActions(s).size());
        if (d.index < tools) {
            s.appPrefs.toolbar_tools = app::RemoveToolbarTool(s.appPrefs.toolbar_tools, static_cast<size_t>(d.index));
            changed = true;
        } else if (d.index >= pins && d.index - pins < static_cast<int>(s.appPrefs.toolbar_apps.size())) {
            s.appPrefs.toolbar_apps.erase(s.appPrefs.toolbar_apps.begin() + (d.index - pins));
            changed = true;
        }
    }
    if (changed && !s.isolatedTest) s.appPrefs.Save();
    PushToolbarPinDrag(s);
    if (!d.active && !cancel) RunToolbarAction(s, true, d.index);   // a click
    ui::ToolbarPickerActivate();
}

// Toolbar toggle tools: the same change and effects as the settings page.
static void FlipToolbarTool(AppState& s, size_t index) {
    if (index >= app::kToolbarToolCount || !kToolbarTools[index].toggle) return;
    const std::wstring_view id = app::kToolbarToolIds[index];
    if (id == L"sidebar") { ToggleSidebarCollapsed(s); return; }
    if (id == L"theme") { ToggleTheme(s); return; }
    if (id == L"details" || id == L"columns" || id == L"folder_size") { DispatchMenuCommand(s, kToolbarTools[index].command); return; }
    app::AppPrefs& p = s.appPrefs;
    app::SettingsEffect effect = app::SettingsEffect::ListStyle;
    if (id == L"hidden") { p.show_hidden_files = !p.show_hidden_files; effect = app::SettingsEffect::FileVisibility; }
    else if (id == L"protected") { p.show_protected_os_files = !p.show_protected_os_files; effect = app::SettingsEffect::FileVisibility; }
    else if (id == L"compact") { p.row_height = p.row_height <= 28 ? 34 : 28; effect = app::SettingsEffect::RowHeight; }
    else if (id == L"zebra") p.list_zebra_rows = !p.list_zebra_rows;
    else if (id == L"size_bar") p.list_size_bar = !p.list_size_bar;
    else if (id == L"smart_date") p.list_smart_date = !p.list_smart_date;
    else if (id == L"outline") p.list_selection_outline = !p.list_selection_outline;
    else if (id == L"extensions") p.show_file_extensions = !p.show_file_extensions;
    else return;
    if (!s.isolatedTest) p.Save();
    ApplySettingsEffects(s, effect);
}

void ShowAdvancedSearch(AppState& s, bool require_scope) {
    if (!s.appPrefs.search_enabled) return;
    app::Tab* tab = ActiveTab(s);
    std::wstring current;
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        current = path::StripExtendedPathPrefix(tab->current_path);
    std::wstring rest;
    if (tab) {
        std::wstring kind;
        app::ParsePulsePath(tab->current_path, &kind, &rest);
        if (kind != L"search") rest.clear();
    }
    app::AdvancedSearchSpec spec = app::ParseSearchQuery(rest, current);
    if (s.addressSearching && IsWindow(s.hwndAddressEdit)) {
        std::wstring draft(static_cast<size_t>(GetWindowTextLengthW(s.hwndAddressEdit)) + 1, L'\0');
        draft.resize(GetWindowTextW(s.hwndAddressEdit, draft.data(), static_cast<int>(draft.size())));
        if (s.addressSearchContent) {
            spec.content = draft;
            spec.current_folder = s.addressSearchRoot;
            spec.custom_folder.clear();
            spec.location = s.addressSearchCurrent && !spec.current_folder.empty()
                ? app::LocationScope::CurrentFolder : app::LocationScope::Indexed;
        } else {
            spec = app::ParseSearchQuery(app::CompileNameQueryInput(draft,
                s.addressSearchCurrent ? s.addressSearchRoot : L""), current);
        }
    }
    if (spec.location == app::LocationScope::Indexed && !current.empty() &&
        (require_scope || (rest.empty() && !s.addressSearching)))
        spec.location = app::LocationScope::CurrentFolder;
    // Live "about N results" while the form is edited (filename matches only).
    auto count = [&s](HWND dialog, const std::wstring& query) {
        const auto split = app::SplitSearchQueryText(query);
        index::Query q;
        q.needle = app::ApplyContentSearchGuards(split.filename_needle, split);
        q.path_prefix = split.path_prefix;
        q.limit = 1;
        s.advancedCountHwnd = dialog;
        s.advancedCountId = ++s.nextIndexReq;
        q.session_id = s.advancedCountId;
        DispatchIndexSearch(s, q, s.advancedCountId);
    };
    const auto result = ui::ShowAdvancedSearchDialog(s.hwnd, spec, s.darkMode, s.accentColor, count, s.appPrefs.list_selection_outline);
    s.advancedCountId = 0;
    s.advancedCountHwnd = nullptr;
    if (result.accepted)
        NavigateTo(s, app::MakeSearchPath(result.query));
}

void ShowSearchFilterMenu(AppState& s, int chip, RECT control_rect) {
    if (chip >= 3) {
        ShowAdvancedSearch(s, false);
        return;
    }
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring kind, rest;
    app::ParsePulsePath(tab->current_path, &kind, &rest);
    if (kind != L"search") return;
    app::AdvancedSearchSpec spec = app::ParseSearchQuery(rest, {});
    constexpr int kTypeCustomCmd = 9;
    std::vector<ui::FluentMenuItem> items;
    auto add = [&](int cmd, const std::wstring& text, bool checked) {
        ui::FluentMenuItem item;
        item.command = cmd;
        item.text = text;
        item.checked = checked;
        items.push_back(std::move(item));
    };
    if (chip == 0) {
        struct Row { int cmd; l10n::StringId id; index::SearchKind kind; };
        const Row rows[] = {
            {1, l10n::StringId::KindAny, index::SearchKind::Any},
            {2, l10n::StringId::KindFolder, index::SearchKind::Folder},
            {3, l10n::StringId::KindDocument, index::SearchKind::Document},
            {4, l10n::StringId::KindImage, index::SearchKind::Image},
            {5, l10n::StringId::KindVideo, index::SearchKind::Video},
            {6, l10n::StringId::KindAudio, index::SearchKind::Audio},
            {7, l10n::StringId::KindArchive, index::SearchKind::Archive},
            {8, l10n::StringId::KindCode, index::SearchKind::Code},
        };
        for (const auto& row : rows) {
            add(row.cmd, l10n::Get(row.id), spec.kind == row.kind);
        }
        add(kTypeCustomCmd, spec.custom_exts.empty()
            ? l10n::Get(l10n::StringId::KindCustom) : spec.custom_exts,
            spec.kind == index::SearchKind::Custom);
    } else {
        if (chip == 1) {
            add(1, l10n::Get(l10n::StringId::DateAny), spec.date == app::DatePreset::Any);
            add(2, l10n::Get(l10n::StringId::DateToday), spec.date == app::DatePreset::Today);
            add(3, l10n::Get(l10n::StringId::DateYesterday), spec.date == app::DatePreset::Yesterday);
            add(4, l10n::Get(l10n::StringId::DateThisWeek), spec.date == app::DatePreset::ThisWeek);
            add(5, l10n::Get(l10n::StringId::DateThisMonth), spec.date == app::DatePreset::ThisMonth);
            add(6, l10n::Get(l10n::StringId::DateThisYear), spec.date == app::DatePreset::ThisYear);
        } else {
            add(1, l10n::Get(l10n::StringId::SizeAny), spec.size == app::SizePreset::Any);
            add(2, l10n::Get(l10n::StringId::SizeEmpty), spec.size == app::SizePreset::Empty);
            add(3, l10n::Get(l10n::StringId::SizeLt1MB), spec.size == app::SizePreset::Lt1MB);
            add(4, l10n::Get(l10n::StringId::Size1To10MB), spec.size == app::SizePreset::From1To10MB);
            add(5, l10n::Get(l10n::StringId::SizeGt10MB), spec.size == app::SizePreset::Gt10MB);
            add(6, l10n::Get(l10n::StringId::SizeGt100MB), spec.size == app::SizePreset::Gt100MB);
            add(7, l10n::Get(l10n::StringId::SizeGt1GB), spec.size == app::SizePreset::Gt1GB);
        }
    }
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackDropdown(control_rect, std::move(items));
    if (chip == 0) {
        if (cmd == kTypeCustomCmd) {
            if (spec.custom_exts.empty()) {
                ShowAdvancedSearch(s, false);
                return;
            }
            spec.kind = index::SearchKind::Custom;
        } else if (cmd > 0) {
            spec.kind = static_cast<index::SearchKind>(cmd - 1);
            spec.custom_exts.clear();
        } else {
            return;
        }
    } else {
        if (cmd <= 0) return;
        if (chip == 1) spec.date = static_cast<app::DatePreset>(cmd - 1);
        else spec.size = static_cast<app::SizePreset>(cmd - 1);
    }
    NavigateTo(s, app::MakeSearchPath(app::CompileSearchQuery(spec)));
}

// Windows' own JSON file dialog (导出别名 / 导入别名, 标签库).
bool PickJsonFile(HWND owner, bool save, const wchar_t* file_name, std::wstring& out) {
    IFileDialog* dlg = nullptr;
    const HRESULT hr = save
        ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                           reinterpret_cast<void**>(&dlg))
        : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                           reinterpret_cast<void**>(&dlg));
    if (FAILED(hr) || !dlg) return false;
    const COMDLG_FILTERSPEC spec{L"JSON", L"*.json"};
    dlg->SetFileTypes(1, &spec);
    dlg->SetDefaultExtension(L"json");
    if (save && file_name) dlg->SetFileName(file_name);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// 管理别名: every alias (type to filter), click opens the item, the row's ×
// removes the alias; 导出 / 导入 below.
void ShowAliasManager(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto fold = [](std::wstring t) {
        for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c));
        return t;
    };
    for (int round = 0; round < 64; ++round) {
        auto all = app::AliasStore::Instance().All();
        std::sort(all.begin(), all.end(), [](const app::AliasEntry& l, const app::AliasEntry& r) {
            return lstrcmpiW(l.alias.c_str(), r.alias.c_str()) < 0;
        });
        std::vector<std::wstring> shown;
        auto build = [&](const std::wstring& query) {
            std::vector<ui::FluentMenuItem> items;
            shown.clear();
            const std::wstring q = fold(query);
            for (const auto& e : all) {
                if (shown.size() >= 30) break;
                if (!q.empty() && !app::AliasStore::AliasMatches(e.alias, q) &&
                    fold(e.path).find(q) == std::wstring::npos) continue;
                const DWORD attrs = GetFileAttributesW(e.path.c_str());
                ui::FluentMenuItem item;
                item.command = app::CmdAliasOpenBase + static_cast<int>(shown.size());
                item.trailing_command = app::CmdAliasRemoveBase + static_cast<int>(shown.size());
                item.text = e.alias;
                item.glyph = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)
                    ? L"\xE8B7" : L"\xE8A5";
                item.secondary = attrs == INVALID_FILE_ATTRIBUTES;  // the item is gone
                item.shortcut = e.path;
                item.shortcut_inline = true;
                item.tooltip = e.path;
                shown.push_back(e.path);
                items.push_back(std::move(item));
            }
            if (items.empty()) {
                ui::FluentMenuItem none;
                none.text = l10n::Get(all.empty() ? l10n::StringId::NoAliasesYet : l10n::StringId::NoMatches);
                none.enabled = false;
                items.push_back(std::move(none));
            }
            items.back().separator_after = true;
            ui::FluentMenuItem out;
            out.command = app::CmdExportAliases;
            out.text = l10n::Get(l10n::StringId::ExportAliasesEllipsis);
            out.glyph = L"\xEDE1";  // export
            out.enabled = !all.empty();
            items.push_back(std::move(out));
            ui::FluentMenuItem in;
            in.command = app::CmdImportAliases;
            in.text = l10n::Get(l10n::StringId::ImportAliasesEllipsis);
            in.glyph = L"\xE8B5";  // import
            items.push_back(std::move(in));
            return items;
        };
        D2D1_RECT_F addr = s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width()));
        POINT pt{ static_cast<LONG>(std::lround(addr.left)), static_cast<LONG>(std::lround(addr.bottom)) };
        ClientToScreen(s.hwnd, &pt);
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::FilterAliasesPlaceholder));
        const int cmd = s.menu->TrackPopup(pt, build(L""), build, false);
        s.menu->SetFilterPlaceholder({});
        if (cmd >= app::CmdAliasRemoveBase && cmd < app::CmdAliasRemoveBase + 30) {
            const size_t index = static_cast<size_t>(cmd - app::CmdAliasRemoveBase);
            if (index < shown.size()) app::AliasStore::Instance().Set(shown[index], L"");
            InvalidateRect(s.hwnd, nullptr, FALSE);
            if (app::AliasStore::Instance().Empty()) return;
            continue;
        }
        if (cmd >= app::CmdAliasOpenBase && cmd < app::CmdAliasOpenBase + 30) {
            const size_t index = static_cast<size_t>(cmd - app::CmdAliasOpenBase);
            if (index >= shown.size()) return;
            const std::wstring& path = shown[index];
            const DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                NavigateTo(s, path);
                return;
            }
            // A file: its folder, with it selected.
            const std::wstring parent = fs::ParentPath(path);
            if (parent.empty()) return;
            const size_t slash = path.find_last_of(L"\\/");
            NavigateTo(s, parent);
            if (app::Tab* tab = ActiveTab(s)) {
                const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
                tab->pending_selected_names = {leaf};
                tab->pending_selection_revision = tab->selection_revision;
                tab->pending_selected_name = leaf;
                tab->pending_ensure_selection_visible = true;
            }
            return;
        }
        if (cmd == app::CmdExportAliases) {
            std::wstring file;
            if (PickJsonFile(s.hwnd, true, L"pulse-aliases.json", file) && !app::AliasStore::Instance().ExportTo(file))
                MessageBeep(MB_ICONWARNING);
            return;
        }
        if (cmd == app::CmdImportAliases) {
            std::wstring file;
            if (!PickJsonFile(s.hwnd, false, nullptr, file)) return;
            if (app::AliasStore::Instance().ImportFrom(file) < 0) {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            InvalidateRect(s.hwnd, nullptr, FALSE);
            continue;  // show the list with what came in
        }
        return;
    }
}

namespace {

// The command palette's 这次用过的: actions run from it this session, newest
// first. Memory only, shared by the windows of this process.
std::vector<int>& PaletteUsed() {
    static std::vector<int> used;
    return used;
}

void RememberPaletteAction(int action) {
    if (action < 0) return;
    auto& used = PaletteUsed();
    used.erase(std::remove(used.begin(), used.end(), action), used.end());
    used.insert(used.begin(), action);
    if (used.size() > 8) used.resize(8);
}

} // namespace

void ShowOmnibar(AppState& s, OmnibarMode mode) {
    // Editing a path is navigation, not a palette search with an empty-result row.
    if (mode == OmnibarMode::Path) {
        ShowAddressEditor(s);
        return;
    }
    if (!EnsureMenu(s)) return;
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.filterEditing) HideFilterEditor(s, true);

    std::wstring prefill;
    bool select_all = true;
    bool hover_first = true;
    if (mode == OmnibarMode::Command) {
        prefill = L">";
        select_all = false;
    }

    s.addressEditing = true;
    InvalidateRect(s.hwnd, nullptr, FALSE);

    D2D1_RECT_F addr = s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width()));
    POINT tl{ static_cast<LONG>(std::lround(addr.left)),
              static_cast<LONG>(std::lround(addr.top)) };
    POINT br{ static_cast<LONG>(std::lround(addr.right)),
              static_cast<LONG>(std::lround(addr.bottom)) };
    ClientToScreen(s.hwnd, &tl);
    ClientToScreen(s.hwnd, &br);
    RECT anchor{ tl.x, tl.y, br.x, br.y };
    const float addr_w = std::max(1.0f, addr.right - addr.left);
    s.menu->SetAnchorRect(anchor);
    s.menu->SetFilterMinWidth(addr_w / std::max(0.01f, s.scale));
    s.menu->SetInitialFilterText(prefill);
    s.menu->SetSelectAllOnOpen(select_all);
    s.menu->SetHoverFirstOnOpen(hover_first);
    // ">": twelve rows, then the list scrolls.
    if (mode == OmnibarMode::Command) s.menu->SetMaxVisibleRows(12);
    // A typed ">" switches any omnibar but the project list to commands.
    if (mode != OmnibarMode::Project)
        s.menu->SetFilterLead(L">", l10n::Get(l10n::StringId::OmnibarCommand));
    s.menu->SetFilterPlaceholder(l10n::Get(
        mode == OmnibarMode::Command ? l10n::StringId::OmnibarCommand :
        mode == OmnibarMode::Project ? l10n::StringId::OmnibarProject :
                                       l10n::StringId::OmnibarDefault));

    const bool project_only = mode == OmnibarMode::Project;
    std::vector<std::wstring> alias_paths;
    // What every action is now; nothing changes while the palette is open.
    const std::vector<app::actions::ActionView> actions = app::actions::ListActions(s);
    auto rebuild = [&s, project_only, &alias_paths, &actions](const std::wstring& query) {
        const auto parsed = app::ParseOmnibarQuery(query, project_only);
        s.paletteQuery = query;
        bool folders_only = parsed.kind == app::OmnibarQuery::Kind::Project;
        std::wstring prefix;
        if (parsed.kind == app::OmnibarQuery::Kind::Project) {
            folders_only = true;
            prefix = ProjectSearchRoot(s);
        }
        const bool run_search = parsed.kind != app::OmnibarQuery::Kind::Command &&
                                (!parsed.needle.empty() || project_only) &&
                                (parsed.kind == app::OmnibarQuery::Kind::Search ||
                                 !app::LooksLikeFilesystemPath(parsed.needle));
        const auto compiled = index::ParseQuery(parsed.needle);
        const std::wstring filename_needle = index::FilenameQueryText(parsed.needle);
        const bool skip_live = compiled.content.present() && filename_needle.empty() &&
                               compiled.path_prefix.empty() && !index::QueryHasExtFilter(compiled) &&
                               !index::QueryHasNameFilter(compiled) && !index::QueryHasFolderFilter(compiled);
        if (run_search && !skip_live) {
            const bool same = parsed.needle == s.paletteIssuedNeedle &&
                              prefix == s.paletteIssuedPrefix &&
                              folders_only == s.paletteIssuedFolders;
            if (!same) {
                s.paletteIssuedNeedle = parsed.needle;
                s.paletteIssuedPrefix = prefix;
                s.paletteIssuedFolders = folders_only;
                s.paletteHits.clear();
                s.paletteTotal = 0;
                s.paletteSearching = true;
                index::Query q;
                q.needle = filename_needle.empty() ? parsed.needle : filename_needle;
                q.path_prefix = prefix.empty() ? compiled.path_prefix : prefix;
                q.folders_only = folders_only;
                q.limit = 24;
                q.rank = true;
                s.paletteSearchId = ++s.nextIndexReq;
                DispatchIndexSearch(s, q, s.paletteSearchId);
            }
        } else {
            s.paletteSearchId = ++s.nextIndexReq; // Invalidate results from the previous mode.
            s.paletteIssuedNeedle.clear();
            s.paletteIssuedPrefix.clear();
            s.paletteIssuedFolders = false;
            s.paletteHits.clear();
            s.paletteTotal = 0;
            s.paletteSearching = false;
        }
        const std::wstring current = ActiveTab(s) ? ActiveTab(s)->current_path : L"";
        std::vector<ui::FluentMenuItem> commands;
        if (parsed.kind == app::OmnibarQuery::Kind::Command ||
            (parsed.kind == app::OmnibarQuery::Kind::Mixed && !app::LooksLikeFilesystemPath(parsed.needle)))
            commands = app::actions::PaletteRows(actions, parsed.needle,
                parsed.kind == app::OmnibarQuery::Kind::Command ? app::actions::PaletteMode::Commands
                                                                : app::actions::PaletteMode::Mixed,
                PaletteUsed());
        auto items = app::BuildCommandPalette(query, std::move(commands), s.places.RecentFolderPaths(),
                                              s.paletteHits, project_only, s.paletteTotal, current);
        // 别名: aliased folders lead ("fbb" or 发布包 → that folder).
        alias_paths.clear();
        if (parsed.kind == app::OmnibarQuery::Kind::Mixed && !parsed.needle.empty() &&
            !app::LooksLikeFilesystemPath(parsed.needle)) {
            std::vector<ui::FluentMenuItem> aliased;
            for (const auto& e : app::AliasStore::Instance().Suggest(parsed.needle, 6)) {
                ui::FluentMenuItem item;
                item.command = app::CmdAliasBase + static_cast<int>(alias_paths.size());
                item.text = e.alias;
                item.glyph = L"\xE8B7";  // folder
                item.shortcut = e.path;
                item.shortcut_inline = true;
                item.tooltip = e.path;
                alias_paths.push_back(e.path);
                aliased.push_back(std::move(item));
            }
            if (!aliased.empty()) {
                aliased.back().separator_after = !items.empty();
                items.insert(items.begin(), std::make_move_iterator(aliased.begin()),
                             std::make_move_iterator(aliased.end()));
            }
        }
        if (s.paletteSearching) {
            ui::FluentMenuItem wait;
            wait.text = l10n::Get(l10n::StringId::Searching);
            wait.enabled = false;
            items.insert(items.begin(), std::move(wait));
        }
        if (items.empty()) {
            ui::FluentMenuItem none;
            none.text = l10n::Get(query.empty()
                ? l10n::StringId::SearchInputPrompt : l10n::StringId::NoMatches);
            none.enabled = false;
            items.push_back(std::move(none));
        }
        return items;
    };

    POINT pt{ tl.x, br.y };
    const int cmd = s.menu->TrackPopup(pt, rebuild(prefill), rebuild, false);
    s.paletteSearchId = ++s.nextIndexReq;
    s.paletteSearching = false;
    s.paletteIssuedNeedle.clear();
    s.addressEditing = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (cmd >= app::CmdAliasBase && cmd < app::CmdAliasBase + 16) {
        const size_t index = static_cast<size_t>(cmd - app::CmdAliasBase);
        if (index < alias_paths.size()) NavigateTo(s, alias_paths[index]);
        return;
    }
    if (cmd == app::actions::kPaletteAskAssistant) {
        // Fills the assistant's box; the user reads it and presses Enter.
        const auto q = app::ParseOmnibarQuery(s.paletteQuery, project_only);
        AiAssistantOpen(s);
        AiAssistantFillComposer(s, q.needle);
        return;
    }
    if (cmd >= app::actions::kPaletteCmdBase && cmd < app::actions::kPaletteCmdEnd) {
        const int offset = cmd - app::actions::kPaletteCmdBase;
        const int action = offset / app::actions::kPaletteStride;
        RememberPaletteAction(action);
        app::actions::RunAction(s, action, offset % app::actions::kPaletteStride - 1);
        return;
    }
    if (cmd != app::CmdNone) {
        // A choice row (排序 → 名称) runs its menu command as it is.
        RememberPaletteAction(app::actions::ActionOfMenuCommand(s, cmd));
        DispatchMenuCommand(s, cmd);
        return;
    }
    if (!s.menu->LastFilterCommitted()) return;
    const auto q = app::ParseOmnibarQuery(s.menu->LastFilterQuery(), project_only);
    if (q.kind == app::OmnibarQuery::Kind::Search) {
        if (!q.needle.empty()) {
            NavigateTo(s, app::MakeSearchPath(q.needle));
        }
        return;
    }
    if (q.kind == app::OmnibarQuery::Kind::Command) return;
    if (q.needle.empty()) return;
    bool shortcut = false;
    const std::wstring path = AddressNavigationTarget(q.needle, &shortcut);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (shortcut || app::LooksLikeFilesystemPath(q.needle) || attrs != INVALID_FILE_ATTRIBUTES)
        NavigateTo(s, path);
}
void ShowRecyclePlaceMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const bool can_empty = !s.recycle_info.valid || s.recycle_info.items > 0;
    const int cmd = s.menu->TrackPopup(screen_pt, app::BuildRecyclePlaceMenu(can_empty));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}
void ApplyAppWindowChrome(AppState& s) {
    if (!s.hwnd) return;
    const auto effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    s.globalSearchWindow.SetAppearance(s.darkMode, effect, s.appPrefs.background_image, s.accentColor);
    // Any selected image takes over the window base: sampled as material for
    // Mica/Acrylic, drawn as-is when the effect is None.
    const bool sample_image = !s.appPrefs.background_image.empty();
    if (sample_image) {
        ui::ApplyWindowEffect(s.hwnd, ui::WindowEffect::None, s.darkMode);
        s.backdropActive = true;
        return;
    }
    if (ui::SystemSupportsDwmBackdrop()) {
        s.backdropActive = ui::ApplyWindowEffect(s.hwnd, effect, s.darkMode);
        return;
    }
    // Windows 10 has no backdrop attribute: leave the frame solid and let the
    // renderer sample the desktop wallpaper with the same recipes, so the
    // effect tiles change the window here too.
    ui::ApplyWindowEffect(s.hwnd, ui::WindowEffect::None, s.darkMode);
    s.backdropActive = ui::WindowEffectUsesBackdrop(effect) && !ui::DesktopWallpaperPath().empty();
}

// The picker draws its list with the main window's renderer; hand it the
// same list style so rows look identical.
static ui::FolderPickerSpec MakePickerSpec(const AppState& s, ui::PickerMode mode) {
    ui::FolderPickerSpec spec;
    spec.mode = mode;
    spec.selection_outline = s.appPrefs.list_selection_outline;
    spec.row_height_dip = static_cast<float>(
        app::EffectiveRowHeightDip(s.appPrefs.row_height, s.appPrefs.ui_font_scale));
    spec.list_smart_date = s.appPrefs.list_smart_date;
    spec.list_zebra_rows = s.appPrefs.list_zebra_rows;
    spec.list_size_bar = s.appPrefs.list_size_bar;
    spec.thumbnail_badges = s.appPrefs.list_thumbnail_badges;
    spec.details_columns = s.appPrefs.details_columns;
    spec.show_hidden = s.appPrefs.show_hidden_files;
    return spec;
}

bool PickImageFile(AppState& s, std::wstring& path) {
    ui::FolderPickerSpec spec = MakePickerSpec(s, ui::PickerMode::Image);
    return ui::ShowFolderPicker(s.hwnd, spec, s.darkMode, s.accentColor, path);
}

bool PickToolbarAppFile(AppState& s, HWND owner, std::wstring& path) {
    ui::FolderPickerSpec spec = MakePickerSpec(s, ui::PickerMode::File);
    spec.title = l10n::Get(l10n::StringId::ToolbarAppPickerTitle);
    spec.filters = {{l10n::Get(l10n::StringId::ToolbarAppPickerFilter) + L" (*.exe; *.lnk)", L"*.exe;*.lnk"}};
    wchar_t programs[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, 0, programs)))
        spec.initial_path = programs;
    ui::FilePickerResult result;
    if (!ui::ShowFilePicker(owner ? owner : s.hwnd, spec, s.darkMode, s.accentColor, result) || result.paths.empty())
        return false;
    path = result.paths.front();
    return true;
}

bool PickFolder(AppState& s, std::wstring& path, const wchar_t* title, std::wstring_view history_key,
                std::wstring_view initial) {
    ui::FolderPickerSpec spec = MakePickerSpec(s, ui::PickerMode::Folder);
    spec.title = title ? title : L"";
    // Empty when this entry point has no history: the picker's last folder.
    spec.initial_path = initial.empty() ? std::wstring(s.appPrefs.PickerFolder(history_key))
                                        : std::wstring(initial);
    if (!ui::ShowFolderPicker(s.hwnd, spec, s.darkMode, s.accentColor, path)) return false;
    if (!path.empty() && s.appPrefs.RememberPickerFolder(history_key, path)) s.appPrefs.Save();
    return true;
}

D2D1_COLOR_F ResolveAccentColor(const app::AppPrefs& prefs, bool dark) {
    uint32_t rgb = 0;
    if (app::ParseAccentRgb(prefs.accent_rgb, rgb))
        return ui::HexColor(rgb);
    return dark || prefs.accent_follow_system ? ui::GetAccentColor() : ui::HexColor(0x527D70);
}

void ApplyAccentFromPrefs(AppState& s, bool snap_picker) {
    uint32_t rgb = 0;
    const bool custom = app::ParseAccentRgb(s.appPrefs.accent_rgb, rgb);
    const bool follow = !custom && (s.darkMode || s.appPrefs.accent_follow_system);
    if (!custom && !follow) rgb = 0x527D70;
    s.accentColor = ResolveAccentColor(s.appPrefs, s.darkMode);
    s.globalSearchWindow.SetAppearance(s.darkMode, ui::WindowEffectFromId(s.appPrefs.window_effect),
        s.appPrefs.background_image, s.accentColor);
    s.bloom_accent.SetSelection(follow, rgb, snap_picker);
    if (s.menu) s.menu->SetTheme(s.darkMode, s.accentColor);
    if (s.operationWindow) s.operationWindow->SetTheme(s.darkMode, s.accentColor);
}

namespace {

bool QuickPreviewItemAt(AppState& s, const app::Tab& tab, int index, ui::QuickPreviewItem& item) {
    if (!tab.snapshot || index < 0 || index >= static_cast<int>(tab.EntryCount())) return false;
    const fs::DirEntry& entry = tab.EntryAt(static_cast<size_t>(index));
    if (entry.pc_launcher || entry.pc_device) return false;  // 网盘程序入口 / phones: no file to preview
    // Folders preview as a contents listing; not in This PC (whole drives),
    // the recycle bin or other virtual locations.
    if (entry.is_dir && (tab.current_path.empty() || IsRecycleTab(&tab) ||
                         fs::IsVirtualPath(tab.current_path)))
        return false;
    item.path = EntryFullPath(tab, index);
    item.name = entry.name;
    item.attrs = entry.attrs;
    item.size = entry.size;
    item.modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) |
                    entry.mtime.dwLowDateTime;
    if (item.path.empty()) return false;
    item.starred = s.places.IsStarred(item.path);
    item.read_only = IsRecycleTab(&tab) || tab.net_readonly;
    return true;
}

// Row of the previewed entry in the focused tab, or -1 once it is gone.
// Names are compared first so the full-path build only runs on candidates.
int QuickPreviewEntryIndex(const app::Tab& tab, const ui::QuickPreviewItem& item) {
    if (!tab.snapshot || item.path.empty()) return -1;
    if (tab.selected_index >= 0 && EntryFullPath(tab, tab.selected_index) == item.path)
        return tab.selected_index;
    const int count = static_cast<int>(tab.EntryCount());
    for (int i = 0; i < count; ++i) {
        if (tab.EntryAt(static_cast<size_t>(i)).name != item.name) continue;
        if (EntryFullPath(tab, i) == item.path) return i;
    }
    return -1;
}

} // namespace

bool QuickPreviewItemForRow(AppState& s, const app::Tab& tab, int index, ui::QuickPreviewItem& item) {
    return QuickPreviewItemAt(s, tab, index, item);
}

bool SelectedQuickPreviewItem(AppState& s, ui::QuickPreviewItem& item) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !QuickPreviewItemAt(s, *tab, tab->selected_index, item)) return false;
    // "6 / 12" in the preview header: the row among the rows on screen.
    if (s.pane) {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const int view = pane.ViewIndex(tab->selected_index);
        const int count = static_cast<int>(pane.EntryCount());
        if (view >= 0 && view < count) {
            item.position = view + 1;
            item.count = count;
        }
    }
    return true;
}

void ToggleQuickPreview(AppState& s) {
    if (s.quickPreview.visible()) {
        s.quickPreview.Close();
        return;
    }
    if(DeferContentSelection(s,[](AppState& v){ToggleQuickPreview(v);},true)) return;
    ui::QuickPreviewItem item;
    if (SelectedQuickPreviewItem(s, item)) {
        const auto effect = s.appPrefs.background_image.empty()
            ? ui::WindowEffectFromId(s.appPrefs.window_effect)
            : ui::WindowEffect::None;
        // Grow the preview out of the selected item's icon (screen point).
        POINT origin{};
        const POINT* zoom_from = nullptr;
        if (app::Tab* tab = ActiveTab(s); tab && s.pane) {
            ui::PaneViewModel pane;
            app::FillPaneViewModel(pane, *s.pane, &s.places);
            const int view = pane.ViewIndex(tab->selected_index);
            if (view >= 0) {
                const D2D1_RECT_F bounds = FocusedPaneRect(s);
                const D2D1_RECT_F list = s.renderer.PaneListRect(pane, bounds);
                const D2D1_RECT_F cell = s.renderer.ItemRectInPane(pane, bounds, view);
                const float cy = (cell.top + cell.bottom) * 0.5f;
                const float row_h = cell.bottom - cell.top;
                // Wide rows (details/list): the icon sits at the left edge;
                // icon grids: the cell centre.
                const float cx = cell.right - cell.left > row_h * 3.0f
                    ? cell.left + row_h * 0.8f : (cell.left + cell.right) * 0.5f;
                if (cy >= list.top && cy <= list.bottom) {
                    origin = POINT{static_cast<LONG>(cx), static_cast<LONG>(cy)};
                    ClientToScreen(s.hwnd, &origin);
                    zoom_from = &origin;
                }
            }
        }
        s.quickPreview.Initialize(s.hwnd, WM_QUICK_PREVIEW_NAVIGATE,
                                  WM_QUICK_PREVIEW_OPEN, WM_QUICK_PREVIEW_COMMAND);
        s.quickPreview.SetAccent(s.accentColor);
        s.quickPreview.Show(item, s.darkMode, effect, s.safeMode, zoom_from);
        s.quickPreviewSelection.Reset(item.path);
    }
}

void NavigateQuickPreview(AppState& s, int direction) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    ui::WindowViewModel vm = BuildVm(s);
    const int count = static_cast<int>(vm.pane.EntryCount());
    if (count <= 0) return;
    int view = vm.pane.ViewIndex(tab->selected_index);
    if (view < 0) view = 0;
    view = std::clamp(view + (direction < 0 ? -1 : 1), 0, count - 1);
    tab->MoveFocus(vm.pane.SourceIndex(view), false);
    EnsureRowVisible(s, *tab, tab->selected_index);
    auto update=[](AppState& v) {
        ui::QuickPreviewItem item;
        if(v.quickPreview.visible() && SelectedQuickPreviewItem(v,item)) v.quickPreview.Update(item);
    };
    if(!DeferContentSelection(s,update,true)) update(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void SyncQuickPreview(AppState& s) {
    if (!s.quickPreview.visible()) {
        s.quickPreviewAnchorView = -1;
        return;
    }
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->loading) return;
    const ui::QuickPreviewItem shown = s.quickPreview.item();
    const int index = QuickPreviewEntryIndex(*tab, shown);
    if (index >= 0) {
        s.quickPreviewAnchorView = -1;
        ui::QuickPreviewItem fresh;
        if (!QuickPreviewItemAt(s, *tab, index, fresh)) return;
        if (fresh.modified != shown.modified || fresh.size != shown.size ||
            fresh.attrs != shown.attrs) {
            s.quickPreview.Update(fresh);  // content changed on disk: reload
        } else {
            s.quickPreview.SetStarred(fresh.starred);
        }
        return;
    }
    // The previewed entry left the listing. After an in-preview delete step
    // to the nearest remaining file at the same view row; otherwise close.
    const int anchor = s.quickPreviewAnchorView;
    s.quickPreviewAnchorView = -1;
    if (anchor >= 0) {
        const ui::WindowViewModel vm = BuildVm(s);
        const int count = static_cast<int>(vm.pane.EntryCount());
        if (count > 0) {
            const int start = std::clamp(anchor, 0, count - 1);
            const auto try_row = [&](int view) {
                ui::QuickPreviewItem next;
                const int source = vm.pane.SourceIndex(view);
                if (!QuickPreviewItemAt(s, *tab, source, next)) return false;
                tab->SelectOnly(source);
                EnsureRowVisible(s, *tab, source);
                s.quickPreview.Update(next);
                InvalidateRect(s.hwnd, nullptr, FALSE);
                return true;
            };
            for (int view = start; view < count; ++view) if (try_row(view)) return;
            for (int view = start - 1; view >= 0; --view) if (try_row(view)) return;
        }
    }
    s.quickPreview.Close();
}

void EnsureEditVisuals(AppState& s);

// The hosted edits use a GDI font sized from the typography settings.
static void RefreshEditFonts(AppState& s) {
    if (s.editFont) {
        DeleteObject(s.editFont);
        s.editFont = nullptr;
    }
    EnsureEditVisuals(s);
    if (s.hwndAddressEdit)
        SendMessageW(s.hwndAddressEdit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), TRUE);
    if (s.hwndRenameEdit)
        SendMessageW(s.hwndRenameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), TRUE);
    if (s.hwndTagRenameEdit)
        SendMessageW(s.hwndTagRenameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), TRUE);
}

void ApplySettingsEffects(AppState& s, app::SettingsEffect effects) {
    // Cheap when unchanged; 文件夹最近变化 (ChangeTracking) also feeds it.
    s.renderer.SetMtimeHint(app::MtimeHintFromPrefs(s.appPrefs));
    s.renderer.SetItemClick(s.appPrefs.single_click_open, s.appPrefs.name_highlight, s.appPrefs.item_checkboxes);
    if (app::HasEffect(effects, app::SettingsEffect::FileVisibility)) {
        ForEachPane(s, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab()) {
                tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
                tab->SetShowProtectedOsFiles(s.appPrefs.show_protected_os_files);
            }
        });
        s.scrollTargetY = 0.0f;
    }
    if (app::HasEffect(effects, app::SettingsEffect::Accent))
        ApplyAccentFromPrefs(s, false);
    if (app::HasEffect(effects, app::SettingsEffect::WindowMaterial)) {
        s.renderer.InvalidateWallpaper();
        ApplyAppWindowChrome(s);
    }
    if (app::HasEffect(effects, app::SettingsEffect::MenuMaterial))
        ui::FluentMenu::SetAcrylicEnabled(s.appPrefs.menu_acrylic && !s.safeMode);
    if (app::HasEffect(effects, app::SettingsEffect::UiFontSize)) {
        ui::typography::SetUiFontScale(s.appPrefs.ui_font_scale);
        // Formats and cached widths carry the previous size.
        ui::typography::InvalidateCaches();
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.InvalidateTypography();
        RefreshEditFonts(s);
    }
    if (app::HasEffect(effects, app::SettingsEffect::RowHeight) ||
        app::HasEffect(effects, app::SettingsEffect::UiFontSize)) {
        const int row_height = app::EffectiveRowHeightDip(s.appPrefs.row_height, s.appPrefs.ui_font_scale);
        s.renderer.SetRowHeightDip(static_cast<float>(row_height));
        if (s.menu)
            s.menu->SetRowHeightDip(static_cast<float>(app::MenuRowHeightDip(row_height)));
    }
    if (app::HasEffect(effects, app::SettingsEffect::ListStyle)) {
        ui::SetHideKnownExtensions(!s.appPrefs.show_file_extensions);
        s.renderer.SetListStyle(s.appPrefs.list_smart_date, s.appPrefs.list_zebra_rows,
                                s.appPrefs.list_size_bar, s.appPrefs.list_tag_name_color,
                                s.appPrefs.list_selection_outline);
        s.renderer.SetTreeSticky(s.appPrefs.list_tree_sticky);
        s.renderer.SetDetailsColumns(s.appPrefs.details_columns);
        s.renderer.SetRowActions(app::RowActionMask(s.ctxMenuPrefs.builtin_hidden,
            s.ctxMenuPrefs.BuiltinOrder(app::BuiltinMenuSurface::RowButtons)));
        s.renderer.SetThumbnailBadges(s.appPrefs.list_thumbnail_badges);
    }
    if (app::HasEffect(effects, app::SettingsEffect::FolderSort)) {
        app::SetFolderSortMode(app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode));
        // Cached snapshots hold the previous order; drop and re-sort what is visible.
        for (const auto& path : VisibleFolderPaths(s)) {
            s.store.MarkDirty(path);
            RefreshPath(s, path, RefreshReason::Explicit);
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    if (app::HasEffect(effects, app::SettingsEffect::TextRendering)) {
        ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(s.appPrefs.text_render));
        s.compositor.UpdateTextRenderingParams(nullptr);
        // Cached widths were measured by the previous rasterizer.
        ui::typography::InvalidateCaches();
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.InvalidateTypography();
    }
    if (app::HasEffect(effects, app::SettingsEffect::TrayDeckIcon))
        s.renderer.SetTrayIconDip(static_cast<float>(s.appPrefs.tray_icon_size));
    if (app::HasEffect(effects, app::SettingsEffect::TrayVisibility))
        s.tray_controller.SetVisible(WantsTrayIcon(s, s.hidden_to_tray));
    if (app::HasEffect(effects, app::SettingsEffect::GlobalSearch))
        ApplyGlobalSearchSettings(s);
    if (app::HasEffect(effects, app::SettingsEffect::StatusBarPerformance))
        s.showFps = s.forceStatusPerformance || s.appPrefs.show_status_performance;
    if (app::HasEffect(effects, app::SettingsEffect::Language)) {
        l10n::SetLanguage(s.appPrefs.language);
        RefreshSidebarModel(s);
        ui::typography::InvalidateCaches();
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.InvalidateTypography();
        RefreshEditFonts(s);
        if (s.pane) {
            ForEachPane(s, [&](app::Pane& pane) {
                if (IsSettingsTab(pane.ActiveTab()))
                    pane.ActiveTab()->virtual_title = l10n::Get(l10n::StringId::Settings);
            });
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

app::SettingsTaskCompletion SettingsCompletion(HWND hwnd) {
    return [hwnd](app::SettingsTaskResult result) {
        auto* message = new app::SettingsTaskResult(std::move(result));
        if (!PostMessageW(hwnd, WM_SETTINGS_TASK_RESULT, 0,
                          reinterpret_cast<LPARAM>(message))) delete message;
    };
}
void ToggleTheme(AppState& s) {
    SetThemeMode(s, s.darkMode ? 1 : 2);
}
void SetThemeMode(AppState& s, int mode) {
    // Circular reveal (motion v2 "theme"): keep the frame the old theme
    // painted, then let the new one grow over it from the pointer.
    const ui::ThemeMode next = mode == 1 ? ui::ThemeMode::Light : mode == 2 ? ui::ThemeMode::Dark : ui::ThemeMode::Auto;
    const bool reveal = s.hwnd && IsWindowVisible(s.hwnd) && !IsIconic(s.hwnd) &&
        ui::motion::SystemAnimationsEnabled() && !ui::IsHighContrast() && !s.shot_high_contrast &&
        ui::ShouldUseDarkMode(next) != s.darkMode;
    if (reveal) {
        s.compositor.CaptureNextFrame();
        RedrawWindow(s.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        s.compositor.CancelCaptureNextFrame();
        if (s.compositor.HeldFrame()) {
            RECT rc{};
            GetClientRect(s.hwnd, &rc);
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(s.hwnd, &pt);
            if (!PtInRect(&rc, pt)) pt = POINT{rc.right / 2, rc.bottom / 2};
            s.themeReveal.start = ui::motion::NowMs();
            s.themeReveal.x = static_cast<float>(pt.x);
            s.themeReveal.y = static_cast<float>(pt.y);
            if (s.framePump.Running()) s.framePump.Arm();
        }
    }
    s.appPrefs.theme_mode = std::clamp(mode, 0, 2);
    s.appPrefs.Save();
    s.themeOverride = mode == 1 ? ui::ThemeMode::Light : mode == 2 ? ui::ThemeMode::Dark : ui::ThemeMode::Auto;
    s.darkMode = ui::ShouldUseDarkMode(s.themeOverride);
    ApplyAccentFromPrefs(s, true);
    ApplyAppWindowChrome(s);
    if (s.editBrush) {
        DeleteObject(s.editBrush);
        s.editBrush = nullptr;
    }
    EnsureEditVisuals(s);
    if (s.hwndAddressEdit) InvalidateRect(s.hwndAddressEdit, nullptr, TRUE);
    if (s.hwndRenameEdit) InvalidateRect(s.hwndRenameEdit, nullptr, TRUE);
    if (s.hwndTagRenameEdit) InvalidateRect(s.hwndTagRenameEdit, nullptr, TRUE);
    if (s.hwndFilterEdit) InvalidateRect(s.hwndFilterEdit, nullptr, TRUE);
    if (s.operationWindow) s.operationWindow->SetTheme(s.darkMode, s.accentColor);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse