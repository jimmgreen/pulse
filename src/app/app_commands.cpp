// app_commands.cpp — extracted from app_main.cpp.
#include "quick_access.h"
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/advanced_search_dialog.h"
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
#include "search_query.h"
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
HANDLE g_shell_watch_stop = nullptr;
HANDLE g_shell_watch_thread = nullptr;

bool EnsureMenu(AppState& s) {
    if (!s.menu) {
        s.menu = std::make_unique<ui::FluentMenu>();
        if (!s.menu->Create(s.hwnd, &s.compositor, s.scale)) {
            s.menu.reset();
            return false;
        }
    }
    s.menu->SetTheme(s.darkMode, s.accentColor);
    return true;
}

void CopySelectedPath(AppState& s) {
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
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
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
    s.pendingRenameName = name;
    s.ops.Submit(std::move(req));
}

void QueueTagAds(AppState& s, std::vector<app::TagAdsUpdate> updates) {
    const HWND notify = s.hwnd;
    s.worker.EnqueueIo([updates = std::move(updates), notify] {
        auto failed = std::make_unique<std::vector<std::wstring>>();
        for (const auto& update : updates) {
            if (app::WriteTagAdsV2(update.path, update.tags)) continue;
            wchar_t volume[MAX_PATH]{};
            if (GetVolumePathNameW(update.path.c_str(), volume, ARRAYSIZE(volume)))
                failed->push_back(ClipboardPath(volume));
            else if (fs::IsUncPath(update.path))
                failed->push_back(L"网络位置");
            else
                failed->push_back(L"此位置");
        }
        if (!failed->empty() && notify)
            PostMessageW(notify, WM_TAG_ADS_WARNING, 0,
                         reinterpret_cast<LPARAM>(failed.release()));
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

void ShowTagPicker(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const std::vector<std::wstring> paths = ActiveTab(s)
        ? SelectedFullPaths(*ActiveTab(s)) : std::vector<std::wstring>{};
    constexpr int kTagPickerBase = 20000;
    constexpr int kCreateTag = 29999;
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
            create.text = L"创建标签“" + query + L"”";
            create.glyph = L"\xE710";
            if (!items.empty()) items.back().separator_after = true;
            items.push_back(std::move(create));
        }
        return items;
    };
    for (;;) {
        s.menu->SetFilterPlaceholder(L"搜索或新建标签…");
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
    s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");
}

void ShowCreateTagPicker(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    constexpr int kCreateTag = 29999;
    constexpr int kColorStrip = 30050;
    constexpr int kColorBase = 30060;
    constexpr int kCustomColor = 30070;
    uint32_t selected_rgb = TagColorPalette(s)[0];
    auto rebuild = [&](const std::wstring& query) {
        const std::vector<uint32_t>& palette = TagColorPalette(s);
        ui::FluentMenuItem colors;
        colors.command = kColorStrip;
        colors.separator_after = true;
        colors.quick_swatches.reserve(palette.size());
        for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
            ui::FluentMenuSwatch swatch;
            swatch.command = kColorBase + i;
            swatch.color = ui::HexColor(palette[static_cast<size_t>(i)]);
            swatch.checked = selected_rgb == palette[static_cast<size_t>(i)];
            colors.quick_swatches.push_back(std::move(swatch));
        }
        std::vector<ui::FluentMenuItem> items;
        items.push_back(std::move(colors));
        ui::FluentMenuItem create;
        create.command = kCreateTag;
        create.text = query.empty() ? L"输入标签名称" : L"创建标签“" + query + L"”";
        create.glyph = L"\xE710";
        create.enabled = !query.empty();
        items.push_back(std::move(create));
        ui::FluentMenuItem custom;
        custom.command = kCustomColor;
        custom.text = L"自定义颜色…";
        custom.has_swatch = true;
        custom.swatch_color = ui::HexColor(selected_rgb);
        custom.separator_after = true;
        items.push_back(std::move(custom));
        return items;
    };
    std::wstring name;
    for (;;) {
        s.menu->SetFilterPlaceholder(L"新建标签…");
        s.menu->SetFilterMinWidth(300.0f);
        s.menu->SetInitialFilterText(name);
        const int command = s.menu->TrackPopup(screen_pt, rebuild(name), rebuild);
        name = s.menu->LastFilterQuery();
        if (command >= kColorBase && command < kColorBase +
            static_cast<int>(TagColorPalette(s).size())) {
            selected_rgb = TagColorPalette(s)[static_cast<size_t>(command - kColorBase)];
            continue;
        }
        if (command == kCustomColor) {
            uint32_t picked = selected_rgb;
            if (ui::ColorPickerPopup::Pick(s.hwnd, &s.compositor, s.menu.get(), s.scale,
                                           screen_pt, picked, s.darkMode, picked)) {
                selected_rgb = picked;
                // Accepted custom colors join the swatch strip (last dot).
                AppendCustomTagColor(s, picked);
            }
            continue;
        }
        if ((command == kCreateTag ||
             (command == app::CmdNone && s.menu->LastFilterCommitted())) && !name.empty()) {
            s.places.CreateTag(name, selected_rgb);
        }
        break;
    }
    s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");
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
    rename.text = L"重命名标签…";
    rename.glyph = L"\xE8AC";
    items.push_back(std::move(rename));
    for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
        const uint32_t rgb = palette[static_cast<size_t>(i)];
        ui::FluentMenuItem color;
        color.command = kColorBase + i;
        if (i < 7) {
            color.text = i == 0 ? L"红色" : i == 1 ? L"橙色" : i == 2 ? L"黄色"
                       : i == 3 ? L"绿色" : i == 4 ? L"蓝色" : i == 5 ? L"紫色" : L"灰色";
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
    remove.text = L"删除标签";
    remove.glyph = L"\xE74D";
    items.push_back(std::move(remove));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kRename) {
        constexpr int kApplyRename = 31300;
        auto rebuild = [](const std::wstring& query) {
            ui::FluentMenuItem item;
            item.command = kApplyRename;
            item.text = query.empty() ? L"输入新名称" : L"重命名为“" + query + L"”";
            item.glyph = L"\xE8AC";
            item.enabled = !query.empty();
            return std::vector<ui::FluentMenuItem>{ std::move(item) };
        };
        s.menu->SetFilterPlaceholder(tag->name);
        s.menu->SetFilterMinWidth(300.0f);
        const int apply = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
        const std::wstring name = s.menu->LastFilterQuery();
        s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");
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
        confirm.title = L"删除标签";
        confirm.confirm_text = L"删除";
        confirm.cancel_text = L"取消";
        confirm.danger = true;
        confirm.message = count == 0
            ? L"删除标签“" + tag->name + L"”？"
            : L"“" + tag->name + L"”已用于 " + std::to_wstring(count)
                + L" 个项目。\n删除后将移除这些关联。";
        if (ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) {
            std::vector<app::TagAdsUpdate> updates;
            s.places.DeleteTag(tag_id, &updates);
            QueueTagAds(s, std::move(updates));
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void DispatchMenuCommand(AppState& s, int cmd) {
    if (cmd >= app::CmdViewBase && cmd < app::CmdViewBase + 8) {
        SetViewMode(s, ui::ViewModeFromIndex(cmd - app::CmdViewBase));
        return;
    }
    switch (cmd) {
    case app::CmdRefresh:
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
    case app::CmdSortAscending:
    case app::CmdSortDescending:
        if (const auto* tab = ActiveTab(s))
            SetSort(s, tab->sort_column, cmd == app::CmdSortAscending
                ? ui::SortDirection::Asc : ui::SortDirection::Desc);
        break;
    case app::CmdOpen: OpenSelected(s); break;
    case app::CmdOpenInNewTab: {
        app::Tab* tab = ActiveTab(s);
        if (!tab || !tab->snapshot) break;
        for (int index : tab->SelectedIndices()) {
            if (index < 0 || index >= static_cast<int>(tab->snapshot->size())) continue;
            const fs::DirEntry& entry = (*tab->snapshot)[static_cast<size_t>(index)];
            if (!entry.is_dir) continue;
            const std::wstring path = EntryFullPath(*tab, index);
            if (!path.empty()) NewTab(s, path);
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
                tab->pending_selected_name = leaf;
                tab->pending_selected_names = { leaf };
                tab->pending_ensure_selection_visible = true;
                NavigateTo(s, parent);
            }
        }
        break;
    }
    case app::CmdCut: CollectToTray(s, true); break;
    case app::CmdCopy: CollectToTray(s, false); break;
    case app::CmdPaste: PasteIntoCurrent(s); break;
    case app::CmdDelete: DeleteSelected(s, (GetKeyState(VK_SHIFT) & 0x8000) != 0); break;
    case app::CmdRename: ShowRenameOverlay(s); break;
    case app::CmdBatchRename: ShowBatchRename(s); break;
    case app::CmdAdvancedSearch: ShowAdvancedSearch(s); break;
    case app::CmdRestoreRecycle: RestoreSelected(s); break;
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
        // Shell verb on the ops pool (plan §6.2); compile-verified in 1B-2.
        std::wstring full;
        if (const app::Tab* tab = ActiveTab(s); IsRecycleTab(tab) && tab->snapshot) {
            const auto indices = tab->SelectedIndices();
            if (!indices.empty()) {
                const fs::DirEntry& entry = (*tab->snapshot)[static_cast<size_t>(indices[0])];
                full = entry.recycle_path.empty() ? entry.full_path : entry.recycle_path;
            }
        } else {
            full = SelectedFullPath(s);
        }
        if (!full.empty()) s.ops.ShowProperties(ClipboardPath(full));
        break;
    }
    case app::CmdOpenTerminal: {
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->current_path.empty()) s.ops.OpenTerminal(ClipboardPath(tab->current_path));
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
    case app::CmdTags: {
        POINT point{};
        GetCursorPos(&point);
        ShowTagPicker(s, point);
        break;
    }
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
                s.places.PinWorkspace(root, L"", static_cast<int>(LayoutOf(s)), CollectPanePaths(s),
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
        const auto split = app::SplitSearchQueryText(q.needle);
        if (split.content.present() && app::ContentSearchNeedsScope(split))
            ShowAdvancedSearch(s, true);
        else
            NavigateTo(s, app::MakeSearchPath(q.needle));
        break;
    }
    case app::CmdInstallFullIndex:
        s.index.RequestInstallService();
        break;
    case app::CmdSettings:
        OpenSettingsTab(s, 0);
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
    return IsClipboardFormatAvailable(CF_HDROP) != 0;
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

std::vector<ui::FluentMenuItem> BuildFinderItemMenu(
        AppState& s, bool can_undo, const std::wstring& undo_label) {
    if (IsRecycleTab(ActiveTab(s)))
        return app::BuildRecycleItemMenu(can_undo, undo_label);
    bool folder = false;
    if (const app::Tab* tab = ActiveTab(s); tab && tab->snapshot) {
        for (int index : tab->SelectedIndices()) {
            if (index >= 0 && index < static_cast<int>(tab->snapshot->size()) &&
                (*tab->snapshot)[static_cast<size_t>(index)].is_dir) {
                folder = true;
                break;
            }
        }
    }
    std::vector<ui::FluentMenuItem> items = app::BuildItemMenu(can_undo, undo_label, folder);
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
        open_path.text = L"打开路径";
        open_path.glyph = L"\xE8B7"; // folder glyph, same family as kGlyphFolder
        items.insert(items.begin() + 1, std::move(open_path));
    }
    if (tab && tab->SelectedCount() >= 2) {
        ui::FluentMenuItem batch;
        batch.command = app::CmdBatchRename;
        batch.text = l10n::Get(l10n::StringId::BatchRename);
        batch.glyph = L"\xE8AC";
        batch.shortcut = L"Ctrl+Shift+R";
        items.insert(items.begin() + 1, std::move(batch));
    }
    return items;
}

// Lowercased common extension of the selection; "" for folders / mixed types.
std::wstring CommonExtension(const app::Tab& tab,
                                    const std::vector<int>& indices) {
    if (!tab.snapshot) return L"";
    std::wstring ext;
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.snapshot->size())) return L"";
        const fs::DirEntry& e = (*tab.snapshot)[static_cast<size_t>(index)];
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
        if (index < 0 || index >= static_cast<int>(tab.snapshot->size())) return L"";
        const fs::DirEntry& e = (*tab.snapshot)[static_cast<size_t>(index)];
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
    if (!s.context_menu.RequestStaticPrefetch(ext)) return;
    HWND hwnd = s.hwnd;
    std::thread([hwnd, ext] {
        auto* result = new ShellVerbsResult{ ext, app::EnumerateStaticVerbs(ext) };
        if (!PostMessageW(hwnd, WM_SHELL_VERBS, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

DWORD WINAPI ShellRegistryWatch(LPVOID param) {
    HWND hwnd = static_cast<HWND>(param);
    HANDLE stop = g_shell_watch_stop;
    if (!stop) return 0;
    struct Watch {
        HKEY key = nullptr;
        HANDLE event = nullptr;
    };
    Watch watches[3]{};
    auto open = [](HKEY root, const wchar_t* sub, Watch& w) {
        if (RegOpenKeyExW(root, sub, 0, KEY_NOTIFY, &w.key) != ERROR_SUCCESS) return;
        w.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (w.event)
            RegNotifyChangeKeyValue(w.key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                    w.event, TRUE);
    };
    open(HKEY_CURRENT_USER, L"Software\\Classes", watches[0]);
    open(HKEY_LOCAL_MACHINE, L"Software\\Classes", watches[1]);
    open(HKEY_CURRENT_USER,
         L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts", watches[2]);
    HANDLE wait[4]{ stop, nullptr, nullptr, nullptr };
    DWORD count = 1;
    for (int i = 0; i < 3; ++i)
        if (watches[i].event) wait[count++] = watches[i].event;
    while (WaitForMultipleObjects(count, wait, FALSE, INFINITE) != WAIT_OBJECT_0) {
        if (!IsWindow(hwnd)) break;
        PostMessageW(hwnd, WM_SHELL_CACHE_INVALIDATE, 0, 0);
        for (int i = 0; i < 3; ++i) {
            if (watches[i].key && watches[i].event)
                RegNotifyChangeKeyValue(watches[i].key, TRUE,
                                        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                        watches[i].event, TRUE);
        }
    }
    for (auto& w : watches) {
        if (w.event) CloseHandle(w.event);
        if (w.key) RegCloseKey(w.key);
    }
    return 0;
}

void StartShellRegistryWatch(HWND hwnd) {
    if (g_shell_watch_thread) return;
    g_shell_watch_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_shell_watch_thread = CreateThread(nullptr, 0, ShellRegistryWatch, hwnd, 0, nullptr);
}

void StopShellRegistryWatch() {
    if (g_shell_watch_stop) SetEvent(g_shell_watch_stop);
    if (g_shell_watch_thread) {
        WaitForSingleObject(g_shell_watch_thread, 2000);
        CloseHandle(g_shell_watch_thread);
        g_shell_watch_thread = nullptr;
    }
    if (g_shell_watch_stop) {
        CloseHandle(g_shell_watch_stop);
        g_shell_watch_stop = nullptr;
    }
}

void SeedShellVerbCache(AppState& s) {
    std::unordered_map<std::wstring, std::vector<app::StaticVerb>> machine;
    if (!app::LoadMachineStaticVerbCache(machine) || machine.empty()) return;
    std::vector<std::wstring> extensions;
    extensions.reserve(machine.size());
    for (const auto& [ext, verbs] : machine) extensions.push_back(ext);
    s.context_menu.MergeStaticCache(std::move(machine));
    HWND hwnd = s.hwnd;
    std::thread([hwnd, extensions = std::move(extensions)] {
        size_t n = 0;
        for (const auto& ext : extensions) {
            if (n++ > 400) break;
            auto* result = new ShellVerbsResult{ ext, app::EnumerateStaticVerbs(ext) };
            if (!PostMessageW(hwnd, WM_SHELL_VERBS, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
        }
    }).detach();
}

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
    while (PeekMessageW(&msg, s.hwnd, WM_SHELLCTX_ITEMS, WM_SHELLCTX_ITEMS, PM_REMOVE) ||
           PeekMessageW(&msg, s.hwnd, WM_SHELL_VERBS, WM_SHELL_VERBS, PM_REMOVE)) {
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
    RefreshActiveTab(s);
    // Bandizip / 7-Zip often return from InvokeCommand before the archive
    // lands on disk; a second pass catches the late create. Unarmed UNC
    // panes poll once a second from the UI timer.
    s.context_menu.ScheduleFolderRefresh(GetTickCount64());
}

// Static verbs first (they were in the first frame), then COM items. A COM
// has_children header plus its following child rows become one nested entry
// (one-level flyout in the menu).
std::vector<ui::FluentMenuItem> ExplorerMenu(
    AppState& s, const std::vector<ui::FluentMenuItem>& base) {
    bool changed = false;
    auto display = s.context_menu.BuildDisplay(s.ctxMenuPrefs, base, changed);
    if (changed) s.ctxMenuPrefs.Save();
    return display;
}

void RefreshOpenCtxMenu(AppState& s) {
    if (!s.context_menu.menu_open() || !s.menu || !s.menu->IsOpen()) return;
    if (s.menu->ReplaceItems(ExplorerMenu(s, s.context_menu.base_items())))
        s.context_menu.NotePatchedDisplay();
}

// Dispatch for the merged Explorer rows. Returns true when cmd was a shell
// row (or the menu was dismissed) and no built-in dispatch should run.
bool HandleShellMenuCommand(AppState& s, int cmd) {
    return s.context_menu.ExecuteShellCommand(cmd,
        [&s] { WaitForShellMenuReady(s); }, [&s] { ScheduleFolderRefresh(s); });
}

void ShowItemContextMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    const std::vector<std::wstring> paths =
        tab ? SelectedFullPaths(*tab) : std::vector<std::wstring>{};
    if (tab && !paths.empty() && !IsRecycleTab(tab))
        StartCtxQuery(s, paths, false, StaticVerbKey(*tab, tab->SelectedIndices()));
    s.context_menu.SeedComItemsFromCache();

    std::wstring undoLabel = s.ops.UndoLabel();
    const auto quick_paths = QuickAccessTargets(tab, false);
    auto base_items = BuildFinderItemMenu(s, s.ops.CanUndo(), undoLabel);
    s.context_menu.OpenMenu(base_items);
    auto display = IsRecycleTab(tab) ? std::move(base_items)
                                     : ExplorerMenu(s, s.context_menu.base_items());
    const int quick_count = std::min(7, static_cast<int>(s.places.tags.size()));

    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    s.context_menu.CloseMenu();
    if (HandleShellMenuCommand(s, cmd)) return;
    if (HandleQuickAccessCommand(s, cmd, quick_paths)) return;
    if (cmd == app::CmdTags) ShowTagPicker(s, screen_pt);
    else if (cmd >= app::CmdTagBase && cmd < app::CmdTagBase + quick_count)
        ToggleTagForSelection(s, s.places.tags[static_cast<size_t>(cmd - app::CmdTagBase)].id, paths);
    else if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

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
    view_options.details_panel = s.showDetailsPanel;
    view_options.can_sort = kind != L"starred" && kind != L"recent";
    view_options.indexed_search = kind == L"search" || kind == L"saved-search";
    view_options.show_path = kind == L"search" || kind == L"saved-search" || kind == L"recycle";
    view_options.filesystem = !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    if (IsRecycleTab(tab)) {
        const bool can_empty = tab->snapshot && !tab->snapshot->empty();
        const std::wstring undoLabel = s.ops.UndoLabel();
        auto items = app::BuildRecycleBackgroundMenu(s.ops.CanUndo(), undoLabel, can_empty);
        app::AppendBackgroundViewCommands(items, view_options);
        s.context_menu.OpenMenu(std::move(items));
        const int cmd = s.menu->TrackPopup(screen_pt, s.context_menu.base_items());
        s.context_menu.CloseMenu();
        if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
        return;
    }
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        StartCtxQuery(s, { tab->current_path }, true, ipc::kBackgroundVerbKey);
    s.context_menu.SeedComItemsFromCache();

    std::wstring undoLabel = s.ops.UndoLabel();
    bool canPaste = !s.tray.batches().empty() || ClipboardHasFiles();
    auto base_items = app::BuildBackgroundMenu(canPaste, s.ops.CanUndo(), undoLabel);
    app::AppendBackgroundViewCommands(base_items, view_options);
    ApplyWorkspacePinLabel(base_items, s);
    const auto quick_paths = QuickAccessTargets(tab, true);
    AppendQuickAccessCommand(s, base_items, quick_paths);
    s.context_menu.OpenMenu(std::move(base_items));
    auto display = ExplorerMenu(s, s.context_menu.base_items());

    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    s.context_menu.CloseMenu();
    if (HandleShellMenuCommand(s, cmd)) return;
    if (HandleQuickAccessCommand(s, cmd, quick_paths)) return;
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

void ShowNewDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    D2D1_RECT_F addr = s.renderer.AddressBarRect((float)s.compositor.Width());
    POINT pt{ (LONG)(addr.right + s.renderer.Margin()),
              (LONG)(s.renderer.TitleBarHeight() + s.renderer.ToolbarHeight()) };
    ClientToScreen(s.hwnd, &pt);
    int cmd = s.menu->TrackPopup(pt, app::BuildNewMenu());
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Stage 1B-2: OLE drag & drop wiring.
void ShowSplitDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    D2D1_RECT_F addr = s.renderer.AddressBarRect((float)s.compositor.Width());
    POINT pt{ (LONG)(addr.right + 220.0f * s.scale),
              (LONG)(s.renderer.TitleBarHeight() + s.renderer.ToolbarHeight()) };
    ClientToScreen(s.hwnd, &pt);
    int cmd = s.menu->TrackPopup(pt, app::BuildSplitMenu(static_cast<int>(LayoutOf(s))));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Browser-style tab groups: named + colored; strip chips open the group popup.
// ---------------------------------------------------------------------------
app::SidebarEntry* QuickAccessEntryForPath(AppState& s,
                                                   const std::wstring& path) {
    for (auto& entry : s.sidebar.quick_access) {
        if (entry.path.empty() || entry.path == app::MakeRecyclePath()) continue;
        if (_wcsicmp(entry.path.c_str(), path.c_str()) == 0)
            return &entry;
    }
    return nullptr;
}

void ShowStarredBadgeEditor(AppState& s, const std::wstring& path,
                                    POINT screen_pt) {
    const app::StarredItem* initial = s.places.FindStarred(path);
    app::SidebarEntry* quick_access = QuickAccessEntryForPath(s, path);
    if ((!initial && !quick_access) || !EnsureMenu(s)) return;
    constexpr int kCustomColor = 30100;
    uint32_t color = initial ? initial->badge_rgb : quick_access->badge_rgb;
    std::wstring current_text = initial ? initial->badge : quick_access->badge;
    s.menu->SetFilterPlaceholder(L"徽章文字（最多 12 个字符）…");
    s.menu->SetInitialFilterText(initial ? initial->badge : quick_access->badge);
    s.menu->SetFilterMinWidth(260.0f);
    auto build = [&](const std::wstring& query) {
        current_text = query.substr(0, 12);
        if (initial) s.places.SetStarredBadge(path, query, color);
        else {
            quick_access->badge = query.substr(0, 12);
            quick_access->badge_rgb = color;
        }
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
        custom.text = L"自定义颜色…";
        custom.has_swatch = true;
        custom.swatch_color = ui::HexColor(color);
        custom.separator_after = true;
        items.push_back(std::move(custom));
        return items;
    };
    auto apply_color = [&](uint32_t next) {
        color = next;
        if (initial) s.places.SetStarredBadge(path, current_text, color);
        else {
            quick_access->badge = current_text;
            quick_access->badge_rgb = color;
        }
    };
    for (;;) {
        const int cmd = s.menu->TrackPopup(screen_pt,
            build(initial ? initial->badge : quick_access->badge),
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
    s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");
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
    open.text = L"打开";
    open.glyph = L"\xE8A0";
    items.push_back(std::move(open));
    if (recent) {
        ui::FluentMenuItem remove;
        remove.command = app::CmdRemoveRecent;
        remove.text = L"从最近使用中移除";
        remove.glyph = L"\xE711";
        items.push_back(std::move(remove));
    } else {
        ui::FluentMenuItem badge;
        badge.command = app::CmdEditStarBadge;
        badge.text = L"编辑徽章";
        badge.glyph = L"\xE8D2";
        items.push_back(std::move(badge));
        const app::StarredItem* starred = s.places.FindStarred(path);
        const app::SidebarEntry* quick_access = QuickAccessEntryForPath(s, path);
        const uint32_t badge_rgb = starred ? starred->badge_rgb
            : quick_access ? quick_access->badge_rgb : 0x0078D4;
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
        custom.text = L"自定义颜色…";
        custom.has_swatch = true;
        custom.swatch_color = ui::HexColor(badge_rgb);
        custom.separator_after = true;
        items.push_back(std::move(custom));
        if (starred) {
            ui::FluentMenuItem remove;
            remove.command = app::CmdRemoveStarred;
            remove.text = L"取消星标";
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
        } else if (auto* quick_access = QuickAccessEntryForPath(s, path)) {
            quick_access->badge_rgb = color;
        }
    } else if (cmd == kCustomColor) {
        const app::StarredItem* starred = s.places.FindStarred(path);
        const app::SidebarEntry* quick_access = QuickAccessEntryForPath(s, path);
        uint32_t picked = starred ? starred->badge_rgb
            : quick_access ? quick_access->badge_rgb : 0x0078D4;
        if (ui::ColorPickerPopup::Pick(s.hwnd, &s.compositor, s.menu.get(),
                                       s.scale, screen_pt, picked, s.darkMode, picked)) {
            AppendCustomTagColor(s, picked);
            if (starred) {
                s.places.SetStarredBadge(path, starred->badge, picked);
                RefreshStarredViews(s);
            } else if (auto* quick = QuickAccessEntryForPath(s, path)) {
                quick->badge_rgb = picked;
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

// Drop groups with no remaining members (after mass closes / leave operations).
void SetViewMode(AppState& s, ui::ViewMode mode) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->view_mode == mode) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    tab->view_mode = mode;
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
    const int cmd = s.menu->TrackPopup(anchor, app::BuildViewMenu(tab->view_mode, s.showDetailsPanel));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
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
    if (spec.location == app::LocationScope::Indexed && !current.empty() &&
        (require_scope || rest.empty()))
        spec.location = app::LocationScope::CurrentFolder;
    const auto result = ui::ShowAdvancedSearchDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    if (result.accepted)
        NavigateTo(s, app::MakeSearchPath(result.query));
}

void ShowSearchFilterMenu(AppState& s, int chip, POINT screen_pt) {
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
    constexpr int kTypeTypedExtCmd = 100;
    auto make_type_items = [&](const std::wstring& query) {
        std::vector<ui::FluentMenuItem> out;
        auto add = [&](int cmd, const std::wstring& text, bool checked) {
            ui::FluentMenuItem item;
            item.command = cmd;
            item.text = text;
            item.radio_group = true;
            item.radio = checked;
            out.push_back(std::move(item));
        };
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
        const std::wstring needle = index::Fold(query);
        for (const auto& row : rows) {
            const std::wstring label = l10n::Get(row.id);
            if (!needle.empty() && index::Fold(label).find(needle) == std::wstring::npos)
                continue;
            add(row.cmd, label, spec.kind == row.kind);
        }
        const std::wstring typed = app::NormalizeExtensionList(query);
        if (!typed.empty()) {
            add(kTypeTypedExtCmd, typed,
                spec.kind == index::SearchKind::Custom &&
                    index::Fold(spec.custom_exts) == index::Fold(typed));
        }
        const std::wstring custom_label = spec.custom_exts.empty()
            ? l10n::Get(l10n::StringId::KindCustom) : spec.custom_exts;
        const bool show_saved = typed.empty() ||
            (!spec.custom_exts.empty() &&
             index::Fold(spec.custom_exts) != index::Fold(typed) &&
             (needle.empty() ||
              index::Fold(custom_label).find(needle) != std::wstring::npos));
        if (show_saved) {
            add(kTypeCustomCmd, custom_label, spec.kind == index::SearchKind::Custom && typed.empty());
        }
        return out;
    };

    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenu::FilterFn filter;
    if (chip == 0) {
        items = make_type_items({});
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::AdvSearchExtHint));
        s.menu->SetFilterMinWidth(220.0f);
        filter = make_type_items;
    } else {
        auto add = [&](int cmd, const std::wstring& text, bool checked) {
            ui::FluentMenuItem item;
            item.command = cmd;
            item.text = text;
            item.radio_group = true;
            item.radio = checked;
            items.push_back(std::move(item));
        };
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
        }
    }
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(items), std::move(filter));
    if (chip == 0) {
        if (cmd == kTypeTypedExtCmd ||
            (cmd <= 0 && s.menu->LastFilterCommitted())) {
            const std::wstring typed = app::NormalizeExtensionList(s.menu->LastFilterQuery());
            if (typed.empty()) return;
            spec.kind = index::SearchKind::Custom;
            spec.custom_exts = typed;
        } else if (cmd == kTypeCustomCmd) {
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
    if (!EnsureMenu(s)) {
        if (mode == OmnibarMode::Path) ShowAddressEditor(s);
        return;
    }
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.filterEditing) HideFilterEditor(s, true);

    app::Tab* tab = ActiveTab(s);
    std::wstring prefill;
    bool select_all = true;
    bool hover_first = true;
    if (mode == OmnibarMode::Command) {
        prefill = L">";
        select_all = false;
    } else if (mode == OmnibarMode::Path) {
        hover_first = false;
        if (tab) {
            prefill = ClipboardPath(tab->current_path);
            if (prefill.empty()) prefill = L"This PC";
        }
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
            const auto split = app::SplitSearchQueryText(q.needle);
            if (split.content.present() && app::ContentSearchNeedsScope(split))
                ShowAdvancedSearch(s, true);
            else
                NavigateTo(s, app::MakeSearchPath(q.needle));
        }
        return;
    }
    if (q.kind == app::OmnibarQuery::Kind::Command) return;
    if (q.needle.empty()) return;
    const std::wstring path = FormatAddressPath(q.needle);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (app::LooksLikeFilesystemPath(q.needle) || attrs != INVALID_FILE_ATTRIBUTES)
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

bool PickImageFile(HWND owner, std::wstring& path) {
    ui::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    COMDLG_FILTERSPEC filters[] = {
        { l10n::Get(l10n::StringId::Images).c_str(), L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.jfif" },
        { l10n::Get(l10n::StringId::AllFiles).c_str(), L"*.*" },
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(l10n::Get(l10n::StringId::TooltipChooseBackground).c_str());
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dialog->Show(owner))) return false;
    ui::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return false;
    PWSTR file = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file)) || !file) return false;
    path.assign(file);
    CoTaskMemFree(file);
    return !path.empty();
}

bool PickFolder(HWND owner, std::wstring& path, const wchar_t* title) {
    ui::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return false;
    dialog->SetTitle(title);
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(owner))) return false;
    ui::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return false;
    PWSTR folder = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &folder)) || !folder) return false;
    path.assign(folder);
    CoTaskMemFree(folder);
    return !path.empty();
}

D2D1_COLOR_F ResolveAccentColor(const app::AppPrefs& prefs) {
    uint32_t rgb = 0;
    if (app::ParseAccentRgb(prefs.accent_rgb, rgb))
        return ui::HexColor(rgb);
    return ui::GetAccentColor();
}

void ApplyAccentFromPrefs(AppState& s, bool snap_picker) {
    uint32_t rgb = 0;
    const bool follow = !app::ParseAccentRgb(s.appPrefs.accent_rgb, rgb);
    s.accentColor = ResolveAccentColor(s.appPrefs);
    s.bloom_accent.SetSelection(follow, rgb, snap_picker);
    if (s.menu) s.menu->SetTheme(s.darkMode, s.accentColor);
    if (s.operationWindow) s.operationWindow->SetTheme(s.darkMode, s.accentColor);
}

bool SelectedQuickPreviewItem(AppState& s, ui::QuickPreviewItem& item) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->selected_index < 0 ||
        tab->selected_index >= static_cast<int>(tab->snapshot->size())) return false;
    const fs::DirEntry& entry = (*tab->snapshot)[static_cast<size_t>(tab->selected_index)];
    if (entry.is_dir) return false;
    item.path = EntryFullPath(*tab, tab->selected_index);
    item.name = entry.name;
    item.attrs = entry.attrs;
    item.size = entry.size;
    item.modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) |
                    entry.mtime.dwLowDateTime;
    return !item.path.empty();
}

void ToggleQuickPreview(AppState& s) {
    if (s.quickPreview.visible()) {
        s.quickPreview.Close();
        return;
    }
    ui::QuickPreviewItem item;
    if (SelectedQuickPreviewItem(s, item)) {
        const auto effect = s.appPrefs.background_image.empty()
            ? ui::WindowEffectFromId(s.appPrefs.window_effect)
            : ui::WindowEffect::None;
        s.quickPreview.Show(item, s.darkMode, effect, s.safeMode);
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
    ui::QuickPreviewItem item;
    if (SelectedQuickPreviewItem(s, item)) s.quickPreview.Update(item);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void EnsureEditVisuals(AppState& s);

void ApplySettingsEffects(AppState& s, app::SettingsEffect effects) {
    if (app::HasEffect(effects, app::SettingsEffect::FileVisibility)) {
        ForEachPane(s, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab()) tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
        });
        s.scrollTargetY = 0.0f;
        s.tagAdsLastSnapshot = nullptr;
    }
    if (app::HasEffect(effects, app::SettingsEffect::Accent))
        ApplyAccentFromPrefs(s, false);
    if (app::HasEffect(effects, app::SettingsEffect::WindowMaterial)) {
        s.renderer.InvalidateWallpaper();
        ApplyAppWindowChrome(s);
    }
    if (app::HasEffect(effects, app::SettingsEffect::RowHeight))
        s.renderer.SetRowHeightDip(static_cast<float>(s.appPrefs.row_height));
    if (app::HasEffect(effects, app::SettingsEffect::TrayDeckIcon))
        s.renderer.SetTrayIconDip(static_cast<float>(s.appPrefs.tray_icon_size));
    if (app::HasEffect(effects, app::SettingsEffect::TrayVisibility))
        s.tray_controller.SetVisible(s.appPrefs.keep_running_on_close);
    if (app::HasEffect(effects, app::SettingsEffect::StatusBarPerformance))
        s.showFps = s.forceStatusPerformance || s.appPrefs.show_status_performance;
    if (app::HasEffect(effects, app::SettingsEffect::Language)) {
        l10n::SetLanguage(s.appPrefs.language);
        s.sidebar = app::BuildSidebarModel(&s.recycle_info);
        SyncSavedSearchSidebar(s);
        ui::typography::InvalidateCaches();
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.InvalidateTypography();
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
    // A title-bar toggle must always produce visible feedback. Cycling through
    // Auto first can resolve to the theme already on screen and appears to
    // require a second click.
    s.themeOverride = s.darkMode ? ui::ThemeMode::Light : ui::ThemeMode::Dark;
    s.darkMode = ui::ShouldUseDarkMode(s.themeOverride);
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
