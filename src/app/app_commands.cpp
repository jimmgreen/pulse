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
#include "window_registry.h"
#include "group_wheel_ui.h"
#include "text_diff.h"
#include "global_search_controller.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/folder_picker_dialog.h"
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
#include "entry_sort.h"
#include "session.h"
#include "context_menu.h"
#include "app_change_tracking.h"
#include "batch_rename.h"
#include "search_query.h"
#include <limits>
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "shell_registry_debounce.h"
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
#include <cwctype>
#include <cstring>
#include <thread>
#include <unordered_set>

using namespace pulse;

namespace pulse {
HANDLE g_shell_watch_stop = nullptr;
HANDLE g_shell_watch_thread = nullptr;
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
    const auto task_id = s.ops.Submit(std::move(req));
    app::QueueCreateRenameIntent(tab->create_rename_intents, task_id,
        tab->current_path, tab->view_generation);
}

void QueueTagAds(AppState& s, std::vector<app::TagAdsUpdate> updates) {
    const HWND notify = s.hwnd;
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

void DispatchMenuCommand(AppState& s, int cmd) {
    const bool needs_files=cmd==app::CmdProperties || cmd==app::CmdOpenPath || cmd==app::CmdOpenInNewTab || cmd==app::CmdRename;
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
    case app::CmdRename: ShowRenameOverlay(s); break;
    case app::CmdBatchRename: ShowBatchRename(s); break;
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
    if (!s.context_menu.RequestStaticPrefetch(ext)) return;
    HWND hwnd = s.hwnd;
    const uint32_t cache_generation = s.context_menu.cache_generation();
    std::thread([hwnd, ext, cache_generation] {
        auto* result = new ShellVerbsResult{ ext, app::EnumerateStaticVerbs(ext), cache_generation };
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
    int watch_of[4]{ -1, -1, -1, -1 };
    DWORD count = 1;
    for (int i = 0; i < 3; ++i) {
        if (!watches[i].event) continue;
        watch_of[count] = i;
        wait[count++] = watches[i].event;
    }
    // A registry cleaner fires these thousands of times in a row; flush once
    // per burst instead of re-seeding the verb cache on every change (#127).
    app::ShellRegistryDebounce debounce;
    for (;;) {
        const DWORD r = WaitForMultipleObjects(count, wait, FALSE,
                                               debounce.WaitMs(GetTickCount64()));
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED || !IsWindow(hwnd)) break;
        if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) {
            // Re-arm only the key that fired: the others are still armed, and
            // re-registering them would stack notifications on the key.
            const Watch& w = watches[watch_of[r - WAIT_OBJECT_0]];
            RegNotifyChangeKeyValue(w.key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                    w.event, TRUE);
            debounce.Note(GetTickCount64());
        }
        // Checked after every wake: during a steady stream the events never
        // let the wait time out, and the max delay must still flush.
        if (debounce.TakeDue(GetTickCount64()))
            PostMessageW(hwnd, WM_SHELL_CACHE_INVALIDATE, 0, 0);
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
    HWND hwnd = s.hwnd;
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
            auto* result = new ShellVerbsResult{ ext, app::EnumerateStaticVerbs(ext), cache_generation };
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
    if(DeferContentSelection(s,[=](AppState& v){ShowItemContextMenu(v,screen_pt);})) return;
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
    if (!IsRecycleTab(tab)) app::ApplyBuiltinMenuPrefs(base_items, s.ctxMenuPrefs, app::BuiltinMenuSurface::Item);
    else app::ApplyBuiltinMenuPrefs(base_items, s.ctxMenuPrefs);
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
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        StartCtxQuery(s, { tab->current_path }, true, ipc::kBackgroundVerbKey);
    s.context_menu.SeedComItemsFromCache();

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
    s.context_menu.OpenMenu(std::move(base_items));
    auto display = ExplorerMenu(s, s.context_menu.base_items());

    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    s.context_menu.CloseMenu();
    if (HandleShellMenuCommand(s, cmd)) return;
    if (HandleQuickAccessCommand(s, cmd, quick_paths)) return;
    if (cmd == app::CmdViewRecentChanges) { OpenChangeView(s, recent_changes_path); return; }
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

void ShowNewDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto anchor = s.renderer.NewCommandRect((float)s.compositor.Width()); POINT pt{(LONG)anchor.left, (LONG)anchor.bottom};
    ClientToScreen(s.hwnd, &pt);
    int cmd = s.menu->TrackPopup(pt, app::BuildNewMenu());
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
    const auto layout = s.renderer.ToolbarLayoutAt(width,s.renderer.NewButtonWidthPx(width-left < 600*s.scale));
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
        const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale));
        POINT anchor{static_cast<LONG>(layout.group.left), static_cast<LONG>(layout.group.bottom)};
        ClientToScreen(s.hwnd, &anchor);
        const int cmd = s.menu->TrackPopup(anchor, app::BuildGroupMenu(options).children);
        if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
    }
}

void ShowToolbarMore(AppState& s) {
    if (!EnsureMenu(s)) return;
    auto* tab=ActiveTab(s);
    if (!tab) return;
    std::vector<ui::FluentMenuItem> items;
    const int commands[]={app::CmdCut,app::CmdCopy,app::CmdPaste,app::CmdRename,app::CmdDelete};
    const l10n::StringId labels[]={l10n::StringId::Cut,l10n::StringId::Copy,l10n::StringId::Paste,
        l10n::StringId::Rename,l10n::StringId::Delete};
    const wchar_t* glyphs[]={L"\xE8C6",L"\xE8C8",L"\xE77F",L"\xE8AC",L"\xE74D"};
    const wchar_t* shortcuts[]={L"Ctrl+X",L"Ctrl+C",L"Ctrl+V",L"F2",L"Delete"};
    for (int i=0;i<5;++i) {
        ui::FluentMenuItem item;
        item.command=commands[i]; item.text=l10n::Get(labels[i]); item.glyph=glyphs[i];
        item.shortcut=shortcuts[i];
        item.enabled=i==2 || tab->SelectedCount()>0;
        items.push_back(std::move(item));
    }
    items.back().separator_after=true;
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
    items.push_back(app::BuildShortcutHints());
    const float width=static_cast<float>(s.compositor.Width());
    const float left=s.renderer.EffectiveSidebarWidth(width);
    const auto layout=s.renderer.ToolbarLayoutAt(width,s.renderer.NewButtonWidthPx(true));
    (void)left;
    POINT anchor{static_cast<LONG>(layout.overflow.left),static_cast<LONG>(layout.overflow.bottom)};
    ClientToScreen(s.hwnd,&anchor);
    const int cmd=s.menu->TrackPopup(anchor,items);
    if (cmd!=app::CmdNone) DispatchMenuCommand(s,cmd);
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
    // Several of the effects above change process-wide state (UI font scale,
    // text rendering, language, folder-sort mode, accent). Every other window
    // renders with the same globals, so it has to rebuild its cached formats
    // and repaint too; PostMessage keeps this out of the setting window's call
    // stack so a window closing mid-broadcast cannot invalidate the iteration.
    for (HWND other : pulse::PulseWindows()) {
        if (other != s.hwnd) PostMessageW(other, WM_PULSE_PREFS_CHANGED, 0, 0);
    }
}

void ApplySharedPreferenceRefresh(AppState& s) {
    s.appPrefs.Load();
    s.ctxMenuPrefs.Load();
    if (s.appPrefs.theme_mode >= 0) {
        s.themeOverride = s.appPrefs.theme_mode == 1 ? ui::ThemeMode::Light :
            s.appPrefs.theme_mode == 2 ? ui::ThemeMode::Dark : ui::ThemeMode::Auto;
    }
    s.darkMode = ui::ShouldUseDarkMode(s.themeOverride);
    s.showFps = s.forceStatusPerformance || s.appPrefs.show_status_performance;
    s.searchHistory.persist = !s.shot.active && !s.menushot && !s.isolatedTest;
    s.renderer.SetRowHeightDip(static_cast<float>(
        app::EffectiveRowHeightDip(s.appPrefs.row_height, s.appPrefs.ui_font_scale)));
    s.renderer.SetListStyle(s.appPrefs.list_smart_date, s.appPrefs.list_zebra_rows,
                            s.appPrefs.list_size_bar, s.appPrefs.list_tag_name_color,
                            s.appPrefs.list_selection_outline);
    s.renderer.SetDetailsColumns(s.appPrefs.details_columns);
    s.renderer.SetRowActions(app::RowActionMask(s.ctxMenuPrefs.builtin_hidden,
        s.ctxMenuPrefs.BuiltinOrder(app::BuiltinMenuSurface::RowButtons)));
    s.renderer.SetThumbnailBadges(s.appPrefs.list_thumbnail_badges);
    s.renderer.SetSidebarWidthDip(static_cast<float>(s.appPrefs.sidebar_width));
    s.renderer.SetVerticalTabs(s.appPrefs.vertical_tabs);
    s.renderer.SetSidebarCollapsed(s.appPrefs.sidebar_collapsed);
    s.renderer.SetTrayIconDip(static_cast<float>(s.appPrefs.tray_icon_size));
    // Formats and cached widths carried the previous scale / rasterizer.
    if (!s.secondary_window) ui::typography::SetUiFontScale(s.appPrefs.ui_font_scale);
    ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(s.appPrefs.text_render));
    ui::typography::InvalidateCaches();
    s.compositor.UpdateTextRenderingParams(nullptr);
    s.renderer.InvalidateWallpaper();
    s.renderer.InvalidateTypography();
    s.compositor.RecreateTextFormats(s.scale);
    ApplyAccentFromPrefs(s, false);
    ApplyAppWindowChrome(s);
    RefreshEditFonts(s);
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
