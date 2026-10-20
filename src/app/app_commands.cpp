#include "tag_color_commands.h"
#include "tag_ads_sync.h"
#include <atomic>
#include "../ui/shortcut_help.h"
// app_commands.cpp — extracted from app_main.cpp.
#include "quick_access.h"
#include "app_column_view.h"
#include "../ui/toolbar_layout.h"
#include "app_internal.h"
#include "app_sidebar_refresh.h"
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
#include "../ui/toolbar_customize_dialog.h"
#include "../ui/advanced_search_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "entry_sort.h"
#include "session.h"
#include "context_menu.h"
#include "system_ctx_registry.h"
#include "shell_verbs.h"
#include "toolbar_apps.h"
#include "app_change_tracking.h"
#include "batch_rename.h"
#include "search_query.h"
#include <limits>
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "shell_registry_watch.h"
#include "search_refresh_log.h"
#include "selection_caps.h"
#include "drive_verbs.h"
#include "bitlocker_unlock.h"
#include "../fs/bitlocker_volume.h"
#include "app_navigation.h"
#include "app_network_locations.h"
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
        text += ClipboardPath(paths[i]);
    }
    ops::WriteClipboardText(text);
}

// Creates "新建文件夹"/"新建文本文档.txt" via the ops layer, then (on the next
// snapshot) selects it and enters the rename overlay.
void CreateNewItem(AppState& s, bool folder) {
    app::Tab* tab = ActiveTab(s);
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
    const std::wstring label = l10n::Pick(L"合并到文件夹", L"new folder with selection");
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
    std::vector<std::wstring> select;
    std::vector<std::wstring> children;
    const uint64_t group = NewUndoGroup();
    const std::wstring label = l10n::Pick(L"解散文件夹", L"ungroup folder");
    for (const std::wstring& folder : folders) {
        if (!ListFolderChildren(folder, children)) continue;
        if (children.empty()) {
            ops::OpRequest recycle;
            recycle.type = ops::OpType::RecycleDelete;
            recycle.sources.push_back(folder);
            recycle.undo_group = group;
            recycle.undo_label = label;
            const auto recycle_id = s.ops.Submit(std::move(recycle));
            s.dissolveSteps.push_back({recycle_id, tab->current_path});
            continue;
        }
        const size_t slash = folder.find_last_of(L"\\/");
        const std::wstring leaf = slash == std::wstring::npos ? folder : folder.substr(slash + 1);
        std::wstring container = folder;
        const bool clash = std::any_of(children.begin(), children.end(), [&](const std::wstring& name) {
            return _wcsicmp(name.c_str(), leaf.c_str()) == 0;
        });
        if (clash) {
            const std::wstring aside = app::UniqueChildName(tab->current_path, leaf, L"");
            ops::OpRequest rename;
            rename.type = ops::OpType::Rename;
            rename.sources.push_back(folder);
            rename.new_name = aside;
            rename.undo_group = group;
            rename.undo_label = label;
            s.ops.Submit(std::move(rename));
            container = JoinChild(tab->current_path, aside);
        }
        ops::OpRequest move;
        move.type = ops::OpType::Move;
        for (const auto& name : children) {
            move.sources.push_back(JoinChild(container, name));
            select.push_back(name);
        }
        move.dest_dir = tab->current_path;
        move.collision_policy = ops::CollisionPolicy::KeepBoth;
        move.undo_group = group;
        move.undo_label = label;
        s.ops.Submit(std::move(move));
        // Queued with the move so one undo always sees the whole action; a
        // child that could not move out keeps its folder.
        ops::OpRequest recycle;
        recycle.type = ops::OpType::RecycleDelete;
        recycle.sources.push_back(container);
        recycle.only_if_empty = true;
        recycle.undo_group = group;
        recycle.undo_label = label;
        const auto recycle_id = s.ops.Submit(std::move(recycle));
        s.dissolveSteps.push_back({recycle_id, tab->current_path});
    }
    if (!select.empty()) {
        tab->pending_selected_name = select.front();
        tab->pending_selected_names = std::move(select);
        tab->pending_selection_revision = tab->selection_revision;
    }
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

void AppendCustomTagColor(AppState& s, uint32_t rgb) {
    std::vector<uint32_t>& palette = TagColorPalette(s);
    if (std::find(palette.begin(), palette.end(), rgb) != palette.end()) return;
    palette.push_back(rgb);
    s.appPrefs.custom_tag_colors.push_back(rgb);
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
    auto rebuild = [&s, &paths](const std::wstring& query) {
        std::vector<ui::FluentMenuItem> items;
        std::wstring needle = query;
        for (auto& c : needle) c = static_cast<wchar_t>(std::towlower(c));
        bool exact = false;
        for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
            const auto& tag = s.places.tags[static_cast<size_t>(i)];
            std::wstring lower = tag.name;
            for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
            if (!needle.empty() && lower.find(needle) == std::wstring::npos) continue;
            exact = exact || (!needle.empty() && lower == needle);
            ui::FluentMenuItem item;
            item.command = kTagPickerBase + i;
            item.text = tag.name;
            item.has_swatch = true;
            item.swatch_color = ui::HexColor(tag.rgb);
            const auto state = s.places.GetSelectionState(tag.id, paths);
            item.checked = state == app::TagSelectionState::All;
            item.mixed = state == app::TagSelectionState::Mixed;
            items.push_back(std::move(item));
        }
        if (!needle.empty() && !exact) {
            ui::FluentMenuItem create;
            create.command = kCreateTag;
            create.text = TagLabel(l10n::StringId::TagCreateFormat, query);
            create.glyph = L"\xE710";
            if (!items.empty()) items.back().separator_after = true;
            items.push_back(std::move(create));
        }
        return items;
    };
    for (;;) {
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TagSearchHint));
        s.menu->SetFilterMinWidth(300.0f);
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

void TrayStartContentCompare(AppState& s) {
    std::vector<std::wstring> paths;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) paths.push_back(item.path);
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
    std::vector<std::wstring> paths;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) paths.push_back(item.path);
    if (paths.size() != 2) return;
    if (!s.textDiff) s.textDiff = std::make_unique<ui::TextDiffWindow>();
    s.textDiff->Show(s.hwnd, paths[0], paths[1], s.darkMode, s.accentColor);
}

void ShowTrayBatchMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    std::vector<std::wstring> paths;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items)
            if (GetFileAttributesW(item.path.c_str()) != INVALID_FILE_ATTRIBUTES)
                paths.push_back(item.path);
    enum { kZip = 1, kRename, kTag, kCopyPaths, kOpenAll, kReveal, kClear, kDiff };
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
    add(kZip, l10n::StringId::TrayMenuZip, L"\xF012", any, false);
    add(kRename, l10n::StringId::TrayMenuRename, L"\xE8AC", paths.size() >= 2, false);
    size_t staged = 0;
    for (const auto& batch : s.tray.batches()) staged += batch.items.size();
    const bool two_text = paths.size() == 2 && staged == 2 &&
                          diff::ProbeLooksText(paths[0]) && diff::ProbeLooksText(paths[1]);
    add(kDiff, l10n::StringId::TrayCompareTwo, L"\xE8AB", two_text, false);
    add(kTag, l10n::StringId::TrayMenuTag, L"\xE8EC", any, true);
    add(kCopyPaths, l10n::StringId::TrayMenuCopyPaths, L"\xE8C8", any, false);
    add(kOpenAll, l10n::StringId::TrayMenuOpenAll, L"\xE8E5", any, false);
    add(kReveal, l10n::StringId::TrayMenuReveal, L"\xE838", any, true);
    add(kClear, l10n::StringId::ClearAll, L"\xE74D", !s.tray.batches().empty(), false);
    // The card tooltip would otherwise linger under the menu.
    s.tooltipText.clear();
    s.hoverSince = 0;
    s.hoverRegion = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    UpdateWindow(s.hwnd);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(items));
    switch (cmd) {
    case kZip:
        ZipViaShell(paths);
        break;
    case kRename:
        ShowBatchRenamePaths(s, paths);
        break;
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
    case kClear: {
        std::vector<std::wstring> all;
        for (const auto& b : s.tray.batches())
            for (const auto& item : b.items) all.push_back(item.path);
        if (!all.empty()) {
            MarkTrayExit(s, all, true);
            SpawnTrayPuffs(s);
        }
        s.tray.Clear();
        break;
    }
    default:
        break;
    }
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

void ShowTagSidebarMenu(AppState& s, const app::TagId& tag_id, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    constexpr int kRename = 31000;
    constexpr int kColorBase = 31100;
    constexpr int kDelete = 31200;
    const std::vector<uint32_t>& palette = TagColorPalette(s);
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem rename;
    rename.command = kRename;
    rename.text = l10n::Get(l10n::StringId::TagRename);
    rename.glyph = L"\xE8AC";
    items.push_back(std::move(rename));
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
        items.push_back(std::move(color));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem remove;
    remove.command = kDelete;
    remove.text = l10n::Get(l10n::StringId::TagDelete);
    remove.glyph = L"\xE74D";
    items.push_back(std::move(remove));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kRename) {
        constexpr int kApplyRename = 31300;
        auto rebuild = [](const std::wstring& query) {
            ui::FluentMenuItem item;
            item.command = kApplyRename;
            item.text = query.empty() ? l10n::Get(l10n::StringId::TagRenameHint)
                                      : TagLabel(l10n::StringId::TagRenameFormat, query);
            item.glyph = L"\xE8AC";
            item.enabled = !query.empty();
            return std::vector<ui::FluentMenuItem>{ std::move(item) };
        };
        s.menu->SetFilterPlaceholder(tag->name);
        s.menu->SetFilterMinWidth(300.0f);
        const int apply = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
        const std::wstring name = s.menu->LastFilterQuery();
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
        // Enter without highlighting the row also confirms the typed name.
        if (apply == kApplyRename ||
            (apply == app::CmdNone && s.menu->LastFilterCommitted())) {
            const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
            if (s.places.RenameTag(tag_id, name))
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
    });
    std::vector<app::TagAdsUpdate> updates;
    s.places.DeleteTag(tag_id, &updates);
    QueueTagAds(s, std::move(updates));
    // Resolve legacy references before deletion changes the catalog indices.
    for (auto* tab : affected) LoadVirtualView(s, *tab, path);
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
    if (!tab.current_path.empty() || !tab.snapshot) return drives;  // This PC rows only
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
    if (error == ERROR_CANCELLED || error == static_cast<DWORD>(-1)) return;
    ui::ConfirmDialogSpec spec;
    spec.tone = ui::ConfirmTone::Warning;
    spec.title = l10n::Pick(L"无法映射网络驱动器", L"Could not map network drive");
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    spec.message = text ? text : l10n::Pick(L"Windows 无法打开网络驱动器对话框。", L"Windows could not open the network drive dialog.");
    if (text) LocalFree(text);
    wchar_t code[64]{};
    swprintf_s(code, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), error);
    spec.note = code;
    spec.confirm_text = l10n::Pick(L"关闭", L"Close");
    (void)ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
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
        spec.title = l10n::Pick(L"断开 ", L"Disconnect ") + drive + l10n::Pick(L"？", L"?");
        spec.message = l10n::Pick(L"此驱动器上有打开的文件或正在运行的程序。强制断开可能会丢失未保存的更改。",
                                  L"Files or programs on this drive are open. Disconnecting anyway may lose unsaved changes.");
        spec.confirm_text = l10n::Pick(L"仍要断开", L"Disconnect anyway");
        spec.danger = true;
        spec.default_choice = ui::ConfirmChoice::Cancel;
        if (ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor))
            app::StartDriveDisconnect(s.hwnd, WM_DRIVE_VERB, result.root, true);
        return;
    }
    const bool eject = result.verb == app::DriveVerb::Eject;
    spec.title = (eject ? l10n::Pick(L"无法弹出 ", L"Cannot eject ") :
        l10n::Pick(L"无法断开网络驱动器 ", L"Cannot disconnect network drive ")) + drive;
    if (eject && result.error == ERROR_DEVICE_IN_USE) {
        spec.message = l10n::Pick(L"设备正在使用中。请关闭可能正在使用该设备上文件的程序或窗口，然后重试。",
                                  L"The device is in use. Close any programs or windows that might be using files on it, then try again.");
        if (!result.veto_name.empty()) spec.note = l10n::Pick(L"占用者：", L"In use by: ") + result.veto_name;
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
    spec.confirm_text = l10n::Pick(L"重试", L"Try again");
    spec.cancel_text = l10n::Pick(L"关闭", L"Close");
    if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return;
    if (eject) {
        uint8_t type = static_cast<uint8_t>(GetDriveTypeW(result.root.c_str()));
        app::StartDriveEject(s.hwnd, WM_DRIVE_VERB, result.root, type);
    } else {
        app::StartDriveDisconnect(s.hwnd, WM_DRIVE_VERB, result.root, false);
    }
}

void DispatchMenuCommand(AppState& s, int cmd) {
    const bool needs_files=cmd==app::CmdProperties || cmd==app::CmdOpenPath || cmd==app::CmdOpenInNewTab || cmd==app::CmdRename ||
        cmd==app::CmdGroupIntoFolder || cmd==app::CmdDissolveFolder;
    if(needs_files && DeferContentSelection(s,[cmd](AppState& v){DispatchMenuCommand(v,cmd);})) return;
    if (cmd >= app::CmdViewBase && cmd < app::CmdViewBase + 8) {
        SetViewMode(s, ui::ViewModeFromIndex(cmd - app::CmdViewBase));
        return;
    }
    switch (cmd) {
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
        if (const auto* tab = ActiveTab(s); tab && !tab->current_path.empty() &&
            !fs::IsVirtualPath(tab->current_path))
            s.ops.ShowProperties(ClipboardPath(tab->current_path));
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
        s.settings.FolderSort(cmd - app::CmdFolderSortTop);
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
        if (!full.empty()) ops::WriteClipboardText(ClipboardPath(full));
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
        s.renderer.SetDetailsPanelVisible(s.showDetailsPanel);
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

// Drives, shares and network locations get their own verbs (弹出, 断开连接,
// 解锁, 映射网络驱动器, 取消固定) instead of the file ones, as in Explorer.
void AdaptItemMenu(AppState& s, std::vector<ui::FluentMenuItem>& items, const app::SelectionCaps& caps) {
    if (TerminalTarget(s).empty()) EraseCommand(items, app::CmdOpenTerminal);
    if (caps.kind == app::SelectionKind::Files || caps.kind == app::SelectionKind::None) return;
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
                verbs.push_back(VerbRow(app::CmdUnlockDrive, l10n::Pick(L"解锁驱动器", L"Unlock drive"), L"\xE785"));
            if (app::DriveEjectable(drive.root, drive.type))
                verbs.push_back(VerbRow(app::CmdEjectDrive, l10n::Pick(L"弹出", L"Eject"), app::EjectGlyph()));
            if (drive.type == DRIVE_REMOTE)
                verbs.push_back(VerbRow(app::CmdDisconnectDrive,
                                        l10n::Pick(L"断开网络驱动器", L"Disconnect network drive"), L"\xE8CD"));
        }
    } else if (caps.kind == app::SelectionKind::NetworkShares) {
        if (!SelectedShare(s).empty())
            verbs.push_back(VerbRow(app::CmdMapNetworkDrive, l10n::Pick(L"映射网络驱动器", L"Map network drive"), L"\xE8CE"));
    } else if (caps.kind == app::SelectionKind::NetworkPlaces) {
        // NetHood shortcuts are local .lnk files: don't offer actions that would
        // accidentally pin the shortcut file as a workspace or Quick Access.
        EraseCommand(items, app::CmdPinWorkspace);
        EraseCommand(items, app::CmdPinQuickAccess);
        EraseCommand(items, app::CmdUnpinQuickAccess);
        if (caps.any_network_pin) {
            verbs.push_back(VerbRow(app::CmdUnpinNetwork, l10n::Get(l10n::StringId::Unpin), L"\xE77A"));
            EraseCommand(items, app::CmdPinNetwork);
        } else {
            const auto paths = tab ? SelectedFullPaths(*tab) : std::vector<std::wstring>{};
            if (std::none_of(paths.begin(), paths.end(), [](const std::wstring& path) { return fs::IsUncPath(path); }))
                EraseCommand(items, app::CmdPinNetwork);
        }
        if (!SelectedShare(s).empty())
            verbs.push_back(VerbRow(app::CmdMapNetworkDrive, l10n::Pick(L"映射网络驱动器", L"Map network drive"), L"\xE8CE"));
    }
    if (verbs.empty()) return;
    verbs.back().separator_after = true;
    // Right after 打开.
    auto open = std::find_if(items.begin(), items.end(),
                             [](const ui::FluentMenuItem& item) { return item.command == app::CmdOpen; });
    items.insert(open == items.end() ? items.begin() : open + 1, verbs.begin(), verbs.end());
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
    const int quick_count = std::min(7, static_cast<int>(s.places.tags.size()));
    ui::FluentMenuItem quick_tags;
    quick_tags.command = app::CmdTags;
    quick_tags.quick_swatches.reserve(static_cast<size_t>(quick_count));
    for (int i = 0; i < quick_count; ++i) {
        const auto& tag = s.places.tags[static_cast<size_t>(i)];
        ui::FluentMenuSwatch swatch;
        swatch.command = app::CmdTagBase + i;
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
        open_path.glyph = L"\xE8B7"; // folder glyph, same family as kGlyphFolder
        items.insert(items.begin() + 1, std::move(open_path));
    }
    if (CanDissolveSelection(tab)) {
        ui::FluentMenuItem dissolve;
        dissolve.command = app::CmdDissolveFolder;
        dissolve.text = l10n::Pick(L"解散文件夹", L"Ungroup folder");
        dissolve.glyph = L"\xED25"; // open folder
        items.insert(items.begin() + 1, std::move(dissolve));
    }
    if (CanGroupSelection(tab)) {
        ui::FluentMenuItem group;
        group.command = app::CmdGroupIntoFolder;
        group.text = l10n::Pick(L"合并到文件夹", L"New folder with selection");
        group.glyph = L"\xE8F4"; // new folder
        items.insert(items.begin() + 1, std::move(group));
    }
    if (tab && tab->SelectedCount() >= 2) {
        ui::FluentMenuItem batch;
        batch.command = app::CmdBatchRename;
        batch.text = l10n::Get(l10n::StringId::BatchRename);
        batch.glyph = L"\xE8AC";
        batch.shortcut = L"Ctrl+Shift+R";
        items.insert(items.begin() + 1, std::move(batch));
    }
    if (tab && SideBySideFolders(*tab).size() == 2) {
        ui::FluentMenuItem compare;
        compare.command = app::CmdCompareSideBySide;
        compare.text = l10n::Get(l10n::StringId::CompareSideBySide);
        compare.glyph = L"\xE89F"; // side-by-side panes
        items.insert(items.begin() + 1, std::move(compare));
    }
    if (tab) AdaptItemMenu(s, items, app::ComputeSelectionCaps(*tab, &s.places));
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
            if (n++ > 400 || g_shell_seed_generation.load() != generation) break;
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
    s.context_menu.StartQuery(s.ctxMenuPrefs, s.hwnd, std::move(paths),
        background, ext, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        ClipboardPath, [&s](const std::wstring& extension) {
            PrefetchStaticVerbs(s, extension);
        });
}

void MaybePrefetchHoverCtxMenu(AppState& s) {
    if (s.context_menu.menu_open() || s.safeMode) return;
    if (s.hoverRow < 0) return;
    const ULONGLONG now = GetTickCount64();
    if (s.ctxHoverSince == 0 || now - s.ctxHoverSince < 150) return;
    app::Pane* pane = s.hoverPaneIndex >= 0 ? PaneAtSlot(s, s.hoverPaneIndex) : s.pane;
    app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab || !tab->snapshot || fs::IsVirtualPath(tab->current_path)) return;
    if (s.hoverRow >= tab->CountBound()) return;
    std::wstring path = EntryFullPath(*tab, s.hoverRow);
    if (path.empty() || path == s.ctxHoverPrefetched) return;
    s.ctxHoverPrefetched = path;
    std::vector<std::wstring> paths;
    std::wstring ext;
    if (tab->IsSelected(s.hoverRow)) {
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
    if (tab && !paths.empty() && !IsRecycleTab(tab))
        StartCtxQuery(s, paths, false, StaticVerbKey(*tab, tab->SelectedIndices()));
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
    const int quick_count = std::min(7, static_cast<int>(s.places.tags.size()));

    if (!IsRecycleTab(tab)) BeginCtxTiming(s, display.size(), seeded);
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
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
    else if (cmd >= app::CmdTagBase && cmd < app::CmdTagBase + quick_count)
        ToggleTagForSelection(s, s.places.tags[static_cast<size_t>(cmd - app::CmdTagBase)].id, paths);
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
    view_options.sort_column = tab->sort_column;
    view_options.sort_direction = tab->sort_direction;
    view_options.folder_sort = s.appPrefs.folder_sort_mode;
    view_options.details_panel = s.showDetailsPanel;
    view_options.can_sort = kind != L"starred" && kind != L"recent";
    view_options.indexed_search = (kind == L"search" || kind == L"saved-search") && !tab->content_results;
    view_options.show_path = kind == L"search" || kind == L"saved-search" || kind == L"recycle";
    view_options.filesystem = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    view_options.group_virtual = kind == L"search" || kind == L"saved-search" || kind == L"tag" || kind == L"recycle";
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
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
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
        for (int command : {app::CmdNewFolder, app::CmdNewTextFile, app::CmdPaste}) EraseCommand(base_items, command);
    }
    if (!view_options.filesystem || app::IsUncServerRoot(tab->current_path)) {
        for (int command : {app::CmdOpenTerminal, app::CmdPinWorkspace}) EraseCommand(base_items, command);
    }
    if (tab->current_path.empty()) {  // This PC, as in Explorer
        auto map = VerbRow(app::CmdMapNetworkDrive, l10n::Pick(L"映射网络驱动器", L"Map network drive"), L"\xE8CE");
        map.separator_after = true;
        base_items.insert(base_items.begin(), std::move(map));
    }
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
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

namespace {
struct ToolbarPinDef {
    int command;
    const wchar_t* glyph;
    l10n::StringId label;
    const wchar_t* shortcut;
};
// Bit i of AppPrefs::toolbar_pins pins kToolbarPins[i] (prototype PINNABLE;
// 新建文件夹 is the New button itself and always shown).
constexpr ToolbarPinDef kToolbarPins[] = {
    {app::CmdNewTextFile, L"\xE8A5", l10n::StringId::NewTextDocument, L""},
    {app::CmdCopyPath, L"\xE71B", l10n::StringId::CopyPath, L"Ctrl+Shift+C"},
    {app::CmdOpenTerminal, L"\xE756", l10n::StringId::OpenInTerminal, L""},
    {app::CmdProperties, L"\xE946", l10n::StringId::Properties, L"Alt+Enter"},
    {app::CmdUndo, L"\xE7A7", l10n::StringId::Undo, L"Ctrl+Z"},
};
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
    const bool can_create = CanCreateHere(ActiveTab(s));
    for (auto& item : items) item.enabled = can_create;
    AppendShellNewRows(items, templates, can_create);
    items.back().separator_after = true;
    items.push_back(CustomizeToolbarItem());
    int cmd = s.menu->TrackPopup(pt, std::move(items));
    if (RunShellNewCommand(s, cmd, templates)) return;
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Stage 1B-2: OLE drag & drop wiring.
void ShowSplitDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto anchor = s.renderer.SplitCommandRect((float)s.compositor.Width()); POINT pt{(LONG)anchor.left, (LONG)anchor.bottom};
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
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem open;
    open.command = app::CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8A0";
    items.push_back(std::move(open));
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
    s.appPrefs.folder_views.ApplyToAll(tab->view_mode);
    s.appPrefs.folder_sorts.ApplyToAll({ tab->sort_column, tab->sort_direction });
    s.appPrefs.Save();
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
void SetViewMode(AppState& s, ui::ViewMode mode) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (s.appPrefs.folder_views.Set(tab->current_path, mode)) s.appPrefs.Save();
    if (tab->view_mode == mode) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    const int old_group = tab->EffectiveGroup();
    tab->view_mode = mode;
    // Grouped order applies to list-like views only; re-sort when that flips.
    if (tab->EffectiveGroup() != old_group && !fs::IsVirtualPath(tab->current_path))
        RefreshActiveTab(s);
    ++tab->view_generation;
    tab->scroll_x = 0.0f;
    tab->scroll_y = 0.0f;
    s.scrollTargetY = 0.0f;
    s.scrollAnimating = false;
    if (tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
    ClampScroll(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
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
        !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path));
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
    options.folder_sort = s.appPrefs.folder_sort_mode;
    options.can_sort = kind != L"starred" && kind != L"recent";
    options.indexed_search = (kind == L"search" || kind == L"saved-search") && !tab->content_results;
    options.show_path = kind == L"search" || kind == L"saved-search" || kind == L"recycle";
    options.group_virtual = kind == L"search" || kind == L"saved-search" || kind == L"tag" || kind == L"recycle";
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
        options.group_virtual = kind == L"search" || kind == L"saved-search" || kind == L"tag" || kind == L"recycle";
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
        uint32_t pins = 0;
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
        cache.pins == s.appPrefs.toolbar_pins)
        return cache.actions;
    cache.state = &s;
    cache.tab = tab;
    cache.path = tab->current_path;
    cache.selected = std::move(selected);
    cache.snapshot = tab->snapshot.get();
    cache.hidden = s.ctxMenuPrefs.builtin_hidden;
    cache.pins = s.appPrefs.toolbar_pins;
    cache.order = std::move(order);
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
            item.command == app::CmdUnpinNetwork || item.command == app::CmdRestoreRecycle;
        if ((builtin == app::BuiltinMenuItem::Count && !selection_row) ||
            builtin == app::BuiltinMenuItem::Undo || builtin == app::BuiltinMenuItem::SelectCommands)
            continue;
        // Already pinned to the toolbar (自定义工具栏): not twice in one row.
        bool pinned = false;
        for (size_t i = 0; i < std::size(kToolbarPins); ++i)
            pinned = pinned || ((cache.pins & (1u << i)) && kToolbarPins[i].command == item.command);
        if (pinned) continue;
        ui::SelectionChipAction action;
        action.command = item.command;
        action.glyph = item.glyph;
        action.label = item.text;
        for (const wchar_t* tail : {L"...", L"\u2026"}) {
            const std::wstring_view t{tail};
            if (action.label.size() > t.size() && std::wstring_view{action.label}.ends_with(t))
                action.label.resize(action.label.size() - t.size());
        }
        if (!item.shortcut.empty()) action.label += L" (" + item.shortcut + L")";
        cache.actions.push_back(std::move(action));
    }
    // Prototype order (标签, 复制路径 first), the selection-only rows ahead of
    // them; cut / copy / rename / delete already sit on the toolbar. Narrow
    // windows drop actions from the end.
    const auto rank = [](int command) {
        static constexpr int kOrder[] = {
            app::CmdUnlockDrive, app::CmdEjectDrive, app::CmdDisconnectDrive, app::CmdRestoreRecycle,
            app::CmdMapNetworkDrive, app::CmdUnpinNetwork, app::CmdBatchRename, app::CmdGroupIntoFolder,
            app::CmdDissolveFolder, app::CmdCompareSideBySide, app::CmdTags, app::CmdCopyPath,
            app::CmdOpenInNewTab, app::CmdOpenPath, app::CmdPinQuickAccess, app::CmdUnpinQuickAccess,
            app::CmdProperties, app::CmdOpenTerminal, app::CmdViewRecentChanges,
            app::CmdPinWorkspace, app::CmdPinNetwork,
        };
        for (size_t i = 0; i < std::size(kOrder); ++i)
            if (kOrder[i] == command) return static_cast<int>(i);
        return static_cast<int>(std::size(kOrder));
    };
    std::stable_sort(cache.actions.begin(), cache.actions.end(),
                     [&](const ui::SelectionChipAction& a, const ui::SelectionChipAction& b) {
                         return rank(a.command) < rank(b.command);
                     });
    if (cache.actions.size() > static_cast<size_t>(ui::kSelChipMaxActions))
        cache.actions.resize(static_cast<size_t>(ui::kSelChipMaxActions));
    return cache.actions;
}

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
    const D2D1_RECT_F button = chip.buttons[static_cast<size_t>(std::min(action, ui::kSelChipClear))];
    POINT anchor{static_cast<LONG>(button.left), static_cast<LONG>(button.bottom)};
    ClientToScreen(s.hwnd, &anchor);
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
    // More is always there (prototype note 5). Narrow windows also fold the
    // file commands, the pinned commands and the pane buttons in here.
    const bool narrow=layout.commands[0].right <= layout.commands[0].left;
    std::vector<ui::FluentMenuItem> items;
    if (narrow) {
        const int commands[]={app::CmdCut,app::CmdCopy,app::CmdPaste,app::CmdRename,app::CmdDelete};
        const l10n::StringId labels[]={l10n::StringId::Cut,l10n::StringId::Copy,l10n::StringId::Paste,
            l10n::StringId::Rename,l10n::StringId::Delete};
        const wchar_t* glyphs[]={L"\xE8C6",L"\xE8C8",L"\xE77F",L"\xE8AC",L"\xE74D"};
        const wchar_t* shortcuts[]={L"Ctrl+X",L"Ctrl+C",L"Ctrl+V",L"F2",L"Delete"};
        // Same rules as the toolbar buttons (ui_toolbar.cpp).
        const bool can_paste=CanCreateHere(tab) && (!s.tray.batches().empty() || ClipboardHasFiles());
        const app::SelectionCaps caps=SelectionCapsOf(s);
        const bool allowed[]={caps.cut,caps.copy,can_paste,caps.rename,caps.del};
        for (int i=0;i<5;++i) {
            ui::FluentMenuItem item;
            item.command=commands[i]; item.text=l10n::Get(labels[i]); item.glyph=glyphs[i];
            item.shortcut=shortcuts[i];
            item.enabled=allowed[i];
            items.push_back(std::move(item));
        }
    }
    // Pinned commands without room on the toolbar (all of them when narrow).
    const auto pins=ToolbarPinActions(s);
    size_t pin=0;
    for (size_t i=0;i<std::size(kToolbarPins) && pin<pins.size();++i) {
        if (!(s.appPrefs.toolbar_pins & (1u<<i))) continue;
        const size_t slot=pin++;
        if (slot<layout.pins.size() && layout.pins[slot].right>layout.pins[slot].left) continue;
        ui::FluentMenuItem item;
        item.command=kToolbarPins[i].command; item.text=l10n::Get(kToolbarPins[i].label);
        item.glyph=kToolbarPins[i].glyph; item.shortcut=kToolbarPins[i].shortcut;
        item.enabled=pins[slot].enabled;
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
    if (!items.empty()) items.back().separator_after=true;
    if (narrow) {
        ui::FluentMenuItem split;
        split.text=l10n::Get(l10n::StringId::SplitLayout);
        split.children=app::BuildSplitMenu(-1);
        items.push_back(std::move(split));
        ui::FluentMenuItem preview;
        preview.command=app::CmdDetailsPanel;
        preview.text=l10n::Get(l10n::StringId::DetailsPane);
        preview.checked=s.showDetailsPanel;
        items.push_back(std::move(preview));
        ui::FluentMenuItem column;
        column.command=app::CmdColumnLayout;
        column.text=l10n::Get(l10n::StringId::ColumnLayout);
        column.checked=tab->column_layout;
        column.separator_after=true;
        items.push_back(std::move(column));
    }
    ui::FluentMenuItem select_all;
    select_all.command=app::CmdSelectAll; select_all.text=l10n::Get(l10n::StringId::SelectAll);
    select_all.glyph=L"\xE8B3"; select_all.shortcut=L"Ctrl+A";
    select_all.enabled=tab->snapshot && tab->EntryCount()>0;
    items.push_back(std::move(select_all));
    ui::FluentMenuItem terminal;
    terminal.command=app::CmdOpenTerminal; terminal.text=l10n::Get(l10n::StringId::OpenInTerminal);
    terminal.glyph=L"\xE756"; terminal.enabled=!TerminalTarget(s).empty();
    items.push_back(std::move(terminal));
    ui::FluentMenuItem properties;
    properties.command=app::CmdProperties; properties.text=l10n::Get(l10n::StringId::Properties);
    properties.glyph=L"\xE946"; properties.shortcut=L"Alt+Enter";
    properties.enabled=tab->SelectedCount()>0;
    properties.separator_after=true;
    items.push_back(std::move(properties));
    items.push_back(CustomizeToolbarItem());
    items.push_back(app::BuildShortcutHints());
    POINT anchor{static_cast<LONG>(layout.overflow.left),static_cast<LONG>(layout.overflow.bottom)};
    ClientToScreen(s.hwnd,&anchor);
    const int cmd=s.menu->TrackPopup(anchor,items);
    if (RunToolbarAppMenuCommand(s,cmd)) return;
    if (cmd!=app::CmdNone) DispatchMenuCommand(s,cmd);
}

std::vector<ui::SelectionChipAction> ToolbarPinActions(AppState& s) {
    std::vector<ui::SelectionChipAction> out;
    const uint32_t pins = s.appPrefs.toolbar_pins;
    const app::Tab* tab = ActiveTab(s);
    if (!pins || !tab) return out;
    const bool has_selection = tab->SelectedCount() > 0;
    for (size_t i = 0; i < std::size(kToolbarPins); ++i) {
        if (!(pins & (1u << i))) continue;
        const ToolbarPinDef& def = kToolbarPins[i];
        ui::SelectionChipAction action;
        action.command = def.command;
        action.glyph = def.glyph;
        action.label = l10n::Get(def.label);
        if (*def.shortcut) action.label += std::wstring(L" (") + def.shortcut + L")";
        switch (def.command) {
        case app::CmdNewTextFile: action.enabled = CanCreateHere(tab); break;
        case app::CmdCopyPath: action.enabled = has_selection || !tab->current_path.empty(); break;
        case app::CmdOpenTerminal: action.enabled = !TerminalTarget(s).empty(); break;
        case app::CmdProperties: action.enabled = has_selection; break;
        default: action.enabled = s.ops.CanUndo(); break;
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

void ShowToolbarCustomize(AppState& s) {
    ui::ToolbarCustomizeSpec spec;
    spec.title = l10n::Get(l10n::StringId::CustomizeToolbar);
    spec.intro = l10n::Get(l10n::StringId::CustomizeToolbarIntro);
    spec.primary_heading = l10n::Get(l10n::StringId::NewButtonPrimary);
    spec.pin_heading = l10n::Get(l10n::StringId::PinToToolbar);
    spec.done_text = l10n::Get(l10n::StringId::ToolbarDone);
    spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    const l10n::StringId primary[] = {l10n::StringId::NewPrimaryFolder, l10n::StringId::NewPrimaryLast,
                                      l10n::StringId::NewPrimaryMenu};
    for (int i = 0; i < 3; ++i) {
        ui::ToolbarCustomizeRow row;
        row.label = l10n::Get(primary[i]);
        if (i == 0) row.caption = L"Ctrl+Shift+N";
        row.checked = s.appPrefs.toolbar_new_primary == i;
        spec.primary.push_back(std::move(row));
    }
    ui::ToolbarCustomizeRow folder;
    folder.glyph = L"\xE8F4";
    folder.label = l10n::Get(l10n::StringId::NewFolder);
    folder.caption = l10n::Get(l10n::StringId::AlwaysShown);
    folder.locked = true;
    spec.pins.push_back(std::move(folder));
    for (size_t i = 0; i < std::size(kToolbarPins); ++i) {
        ui::ToolbarCustomizeRow row;
        row.glyph = kToolbarPins[i].glyph;
        row.label = l10n::Get(kToolbarPins[i].label);
        row.caption = kToolbarPins[i].shortcut;
        row.checked = (s.appPrefs.toolbar_pins & (1u << i)) != 0;
        spec.pins.push_back(std::move(row));
    }
    FillToolbarAppRows(s, spec);
    if (!ui::ShowToolbarCustomizeDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return;
    int new_primary = 0;
    for (size_t i = 0; i < spec.primary.size(); ++i)
        if (spec.primary[i].checked) new_primary = static_cast<int>(i);
    uint32_t pins = 0;
    for (size_t i = 0; i < std::size(kToolbarPins) && i + 1 < spec.pins.size(); ++i)
        if (spec.pins[i + 1].checked) pins |= 1u << i;
    const bool apps_changed = ApplyToolbarAppRows(s, spec);
    if (apps_changed || new_primary != s.appPrefs.toolbar_new_primary || pins != s.appPrefs.toolbar_pins) {
        s.appPrefs.toolbar_new_primary = new_primary;
        s.appPrefs.toolbar_pins = pins;
        if (!s.isolatedTest) s.appPrefs.Save();
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowAdvancedSearch(AppState& s, bool require_scope) {
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
    s.menu->SetFilterPlaceholder(l10n::Get(
        mode == OmnibarMode::Command ? l10n::StringId::OmnibarCommand :
        mode == OmnibarMode::Project ? l10n::StringId::OmnibarProject :
                                       l10n::StringId::OmnibarDefault));

    const bool project_only = mode == OmnibarMode::Project;
    auto rebuild = [&s, project_only](const std::wstring& query) {
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
        auto items = app::BuildCommandPalette(query, s.places.RecentFolderPaths(), s.paletteHits,
                                              project_only, s.paletteTotal, current);
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
    if (cmd != app::CmdNone) {
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
    s.backdropActive = ui::ApplyWindowEffect(s.hwnd, effect, s.darkMode);
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

bool PickFolder(AppState& s, std::wstring& path, const wchar_t* title) {
    ui::FolderPickerSpec spec = MakePickerSpec(s, ui::PickerMode::Folder);
    spec.title = title ? title : L"";
    return ui::ShowFolderPicker(s.hwnd, spec, s.darkMode, s.accentColor, path);
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
    return tab && QuickPreviewItemAt(s, *tab, tab->selected_index, item);
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
        s.renderer.SetListStyle(s.appPrefs.list_smart_date, s.appPrefs.list_zebra_rows,
                                s.appPrefs.list_size_bar, s.appPrefs.list_tag_name_color,
                                s.appPrefs.list_selection_outline);
        s.renderer.SetDetailsColumns(s.appPrefs.details_columns);
        s.renderer.SetRowActions(app::RowActionMask(s.ctxMenuPrefs.builtin_hidden,
            s.ctxMenuPrefs.BuiltinOrder(app::BuiltinMenuSurface::RowButtons)));
        s.renderer.SetThumbnailBadges(s.appPrefs.list_thumbnail_badges);
        const bool touch_ready = ui::DetectTouchscreenReady();
        s.renderer.SetTouchscreenReady(touch_ready);
        s.renderer.SetTouchDensity(ui::ResolveTouchDensity(s.appPrefs.touch_ui_mode, touch_ready));
        s.renderer.SetCheckboxVisibility(ui::ResolveCheckboxVisibility(s.appPrefs.item_checkboxes));
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
