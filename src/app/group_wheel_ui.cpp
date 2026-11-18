#include "group_wheel_ui.h"
#include "app_internal.h"
#include "context_menu.h"
#include "entry_group.h"
#include "entry_sort.h"
#include "media_columns_ui.h"
#include "window_registry.h"
#include "../common/text_format.h"
#include "../common/localization.h"
#include "../common/string_ids.h"
#include "../ui/command_icons.h"
#include "../ui/group_wheel.h"
#include "../ui/confirm_dialog.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <windowsx.h>
#include <algorithm>
#include <cwctype>
#include <numeric>
#include <unordered_map>

namespace pulse {
namespace {

struct ModeStyle {
    int value;
    l10n::StringId name;
    ui::command_icons::Icon icon;
    uint32_t hue1, hue2;
};

constexpr ModeStyle kModes[] = {
    {0, l10n::StringId::GroupNone, ui::command_icons::Icon::List, 0x94A3B8, 0x475569},
    {1, l10n::StringId::GroupByName, ui::command_icons::Icon::Sort, 0xA78BFA, 0x6D28D9},
    {2, l10n::StringId::GroupByDate, ui::command_icons::Icon::History, 0x34D399, 0x0F766E},
    {3, l10n::StringId::GroupByType, ui::command_icons::Icon::File, 0xFBBF24, 0xB45309},
    {4, l10n::StringId::GroupBySize, ui::command_icons::Icon::Sliders, 0xF472B6, 0xBE185D},
    {5, l10n::StringId::GroupByTag, ui::command_icons::Icon::Tag, 0x60A5FA, 0x1D4ED8},
    {6, l10n::StringId::Location, ui::command_icons::Icon::Folder, 0x22D3EE, 0x0E7490},
};

// Media folders add groups by what the files hold (media::Kind 1 pictures,
// 2 music, 3 videos; app::GroupBy::Media*). Session only, like the list.
struct MediaModeStyle {
    int kind;
    ModeStyle style;
};
constexpr MediaModeStyle kMediaModes[] = {
    {1, {10, l10n::StringId::GroupByTakenMonth, ui::command_icons::Icon::History, 0xFB923C, 0xC2410C}},
    {1, {11, l10n::StringId::ColumnMediaCamera, ui::command_icons::Icon::Camera, 0x38BDF8, 0x0369A1}},
    {2, {12, l10n::StringId::ColumnMediaAlbum, ui::command_icons::Icon::Music, 0xE879F9, 0xA21CAF}},
    {2, {13, l10n::StringId::ColumnMediaArtist, ui::command_icons::Icon::Stack, 0x818CF8, 0x4338CA}},
    {3, {14, l10n::StringId::ColumnMediaResolution, ui::command_icons::Icon::Video, 0xFB923C, 0xC2410C}},
};

std::wstring Format(l10n::StringId id, int n) {
    wchar_t buf[128]{};
    swprintf_s(buf, l10n::Get(id).c_str(), n);
    return buf;
}

// Header text for a group, matching the list's group headers.
std::wstring Label(app::GroupBy by, int rank, const std::wstring& key, const fs::DirEntry& e,
                   const app::TagCatalogSnapshot* tags, const app::GroupClock& clock) {
    using l10n::StringId;
    if (rank == app::kFolderGroupRank) return l10n::Get(StringId::GroupFolders);
    if (app::IsMediaGroup(by)) {
        if (rank < 0) return l10n::Get(StringId::GroupFolders);
        const std::wstring label = app::MediaGroupLabel(clock, rank);
        return label.empty() ? l10n::Get(StringId::GroupMediaUnknown) : label;
    }
    switch (by) {
    case app::GroupBy::Name: {
        static const wchar_t* const kNames[] = {L"0 \x2013 9", L"A \x2013 H", L"I \x2013 P", L"Q \x2013 Z"};
        return rank >= 0 && rank < 4 ? kNames[rank] : l10n::Get(StringId::GroupNameOther);
    }
    case app::GroupBy::Date:
        return l10n::Get(static_cast<StringId>(IDS_GROUP_D_FUTURE + std::clamp(rank, 0, 8)));
    case app::GroupBy::Type: {
        if (rank == 0) return l10n::Get(StringId::GroupFolders);
        std::wstring ext = key.size() > 2 ? key.substr(2) : std::wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(std::towupper(c));
        return ext.empty() ? std::wstring(L"-") : ext;
    }
    case app::GroupBy::Size:
        return l10n::Get(static_cast<StringId>(IDS_GROUP_FOLDERS + std::clamp(rank, 0, 7)));
    case app::GroupBy::Tag:
        if (tags && rank >= 0 && static_cast<size_t>(rank) < tags->tags.size())
            return tags->tags[static_cast<size_t>(rank)].name;
        return l10n::Get(StringId::GroupUntagged);
    case app::GroupBy::Location: {
        const std::wstring full = app::GroupLocationLabel(e);
        const size_t slash = full.find_last_of(L'\\', full.size() > 1 ? full.size() - 2 : 0);
        return slash == std::wstring::npos || slash + 1 >= full.size() ? full : full.substr(slash + 1);
    }
    default:
        return {};
    }
}

ui::GroupWheelOption BuildOption(const app::Tab& tab, const ModeStyle& mode,
                                 const std::vector<fs::DirEntry>& entries) {
    const size_t count = entries.size();
    ui::GroupWheelOption o;
    o.value = mode.value;
    o.icon = static_cast<int>(mode.icon);
    o.name = l10n::Get(mode.name);
    o.hue1 = mode.hue1;
    o.hue2 = mode.hue2;
    if (mode.value == 0) {
        o.pill = l10n::Get(l10n::StringId::GroupWheelFlat);
        o.meta = Format(l10n::StringId::GroupWheelItems, static_cast<int>(count));
        return o;
    }
    const app::GroupBy by = app::GroupByFromInt(mode.value);
    app::GroupClock clock = by == app::GroupBy::Date ? app::MakeGroupClock(tab.sort_column) : app::GroupClock{};
    clock.folders = tab.EffectiveFolderSort();
    if (by == app::GroupBy::Tag) clock.tags = app::TagGroupsForFolder(tab.current_path);
    if (app::IsMediaGroup(by)) clock.media = app::MediaGroupsForFolder(tab.current_path, by);
    const auto catalog = by == app::GroupBy::Tag ? app::CurrentTagCatalog() : nullptr;
    struct Group { std::wstring key; int rank; size_t sample; int count; };
    std::vector<Group> groups;
    std::unordered_map<std::wstring, size_t> index;
    for (size_t i = 0; i < count; ++i) {
        const fs::DirEntry& e = entries[i];
        std::wstring key = app::GroupKey(e, by, clock);
        const auto [it, inserted] = index.try_emplace(key, groups.size());
        if (inserted) groups.push_back({std::move(key), app::GroupRank(e, by, clock), i, 0});
        ++groups[it->second].count;
    }
    std::stable_sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
        return app::GroupCompare(entries[a.sample], entries[b.sample], by, clock,
                                 tab.sort_column, tab.sort_direction) < 0;
    });
    for (const auto& g : groups) {
        if (o.groups.size() >= 8) break;
        o.groups.emplace_back(Label(by, g.rank, g.key, entries[g.sample], catalog.get(), clock), g.count);
    }
    o.pill = Format(l10n::StringId::GroupWheelGroups, static_cast<int>(groups.size()));
    for (size_t i = 0; i < o.groups.size() && i < 3; ++i) {
        if (i) o.meta += L" \x00B7 ";
        o.meta += o.groups[i].first;
    }
    return o;
}

enum class WheelKind { Group, Sort };
WheelKind g_kind = WheelKind::Group;

struct SortStyle {
    ui::SortColumn column;
    l10n::StringId name;
    l10n::StringId hint;        // plain-language second line
    ui::command_icons::Icon icon;
    uint32_t hue1, hue2;
};

constexpr SortStyle kSortColumns[] = {
    {ui::SortColumn::Name, l10n::StringId::ColumnName, l10n::StringId::SortHintName,
     ui::command_icons::Icon::Sort, 0xA78BFA, 0x6D28D9},
    {ui::SortColumn::Mtime, l10n::StringId::ColumnModified, l10n::StringId::SortHintModified,
     ui::command_icons::Icon::History, 0x34D399, 0x0F766E},
    {ui::SortColumn::Created, l10n::StringId::ColumnCreated, l10n::StringId::SortHintCreated,
     ui::command_icons::Icon::Add, 0x60A5FA, 0x1D4ED8},
    {ui::SortColumn::Accessed, l10n::StringId::ColumnAccessed, l10n::StringId::SortHintAccessed,
     ui::command_icons::Icon::Eye, 0xFB923C, 0xC2410C},
    {ui::SortColumn::Type, l10n::StringId::ColumnType, l10n::StringId::SortHintType,
     ui::command_icons::Icon::File, 0xFBBF24, 0xB45309},
    {ui::SortColumn::Size, l10n::StringId::ColumnSize, l10n::StringId::SortHintSize,
     ui::command_icons::Icon::Sliders, 0xF472B6, 0xBE185D},
    {ui::SortColumn::Path, l10n::StringId::ColumnPath, l10n::StringId::SortHintPath,
     ui::command_icons::Icon::Folder, 0x22D3EE, 0x0E7490},
};

std::wstring ShortTime(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{}, now{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"-";
    GetLocalTime(&now);
    wchar_t buf[32]{};
    if (st.wYear == now.wYear)
        swprintf_s(buf, L"%02u-%02u", st.wMonth, st.wDay);   // preview column is narrow
    else
        swprintf_s(buf, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::wstring TypeText(const fs::DirEntry& e) {
    if (e.is_dir) return l10n::Get(l10n::StringId::GroupFolders);
    const size_t dot = e.name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= e.name.size()) return L"-";
    std::wstring ext = e.name.substr(dot + 1);
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towupper(ch));
    return ext;
}

uint32_t Swatch(const fs::DirEntry& e) {
    if (e.is_dir) return 0xE8B64C;
    static constexpr uint32_t kPalette[] = {0x60A5FA, 0xF472B6, 0xFBBF24, 0x34D399, 0xA78BFA, 0xF87171, 0x22D3EE};
    const std::wstring ext = TypeText(e);
    uint32_t h = 2166136261u;
    for (wchar_t ch : ext) h = (h ^ static_cast<uint32_t>(ch)) * 16777619u;
    return kPalette[h % 7];
}

std::wstring SortValue(const fs::DirEntry& e, ui::SortColumn col) {
    switch (col) {
    case ui::SortColumn::Mtime: return ShortTime(e.mtime);
    case ui::SortColumn::Created: return ShortTime(e.ctime);
    case ui::SortColumn::Accessed: return ShortTime(e.atime);
    case ui::SortColumn::Size: return e.is_dir ? std::wstring(L"\x2014") : format::ByteSize(e.size);
    case ui::SortColumn::Path: {
        const std::wstring full = app::GroupLocationLabel(e);
        const size_t slash = full.find_last_of(L'\\', full.size() > 1 ? full.size() - 2 : 0);
        return slash == std::wstring::npos || slash + 1 >= full.size() ? full : full.substr(slash + 1);
    }
    default: return TypeText(e);
    }
}

std::vector<std::wstring> SplitBar(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        const size_t bar = text.find(L'|', start);
        out.push_back(text.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    return out;
}

// Everyday example files for the sort preview (the user's own folder may be
// full of cryptic names). Sorted with the real comparator, so direction and
// folder placement behave exactly like the list.
std::vector<fs::DirEntry> SampleEntries(bool with_path) {
    struct Sample { bool dir; double hours_ago; uint64_t size; int location; };
    static constexpr Sample kSamples[] = {
        {true, 30.0, 0, 1},        {true, 24.0 * 12, 0, 0},  {false, 2.0, 1'240'000, 0},
        {false, 24.0 * 3, 3'400'000, 1}, {false, 24.0 * 40, 48'000, 0},
        {false, 24.0 * 7, 26'000, 3}, {false, 24.0 * 200, 5'100'000, 2},
    };
    const auto names = SplitBar(l10n::Get(l10n::StringId::SortWheelSamples));
    const auto places = SplitBar(l10n::Get(l10n::StringId::SortWheelSamplePlaces));
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = (static_cast<uint64_t>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    std::vector<fs::DirEntry> out;
    for (size_t i = 0; i < std::size(kSamples) && i < names.size(); ++i) {
        const auto& sample = kSamples[i];
        fs::DirEntry e;
        e.name = names[i];
        e.is_dir = sample.dir;
        e.attrs = sample.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        e.size = sample.size;
        const uint64_t t = now - static_cast<uint64_t>(sample.hours_ago * 36'000'000'000.0);
        e.mtime.dwLowDateTime = static_cast<DWORD>(t & 0xFFFFFFFFu);
        e.mtime.dwHighDateTime = static_cast<DWORD>(t >> 32);
        // Plausible creation / access times with their own order, so those
        // previews differ from "modified": created earlier, opened recently.
        const double created_days = static_cast<double>(i * 5 % 7 + 1);
        const double accessed_hours = sample.hours_ago * 0.25 + 3.0 * static_cast<double>(i % 3);
        const uint64_t created = t - static_cast<uint64_t>(24.0 * created_days * 36'000'000'000.0);
        const uint64_t accessed = now - static_cast<uint64_t>(accessed_hours * 36'000'000'000.0);
        e.ctime.dwLowDateTime = static_cast<DWORD>(created & 0xFFFFFFFFu);
        e.ctime.dwHighDateTime = static_cast<DWORD>(created >> 32);
        e.atime.dwLowDateTime = static_cast<DWORD>(accessed & 0xFFFFFFFFu);
        e.atime.dwHighDateTime = static_cast<DWORD>(accessed >> 32);
        if (with_path && static_cast<size_t>(sample.location) < places.size())
            e.full_path = L"C:\\" + places[static_cast<size_t>(sample.location)] + L"\\" + e.name;
        out.push_back(std::move(e));
    }
    return out;
}

void ApplySort(AppState& s, int value, bool desc, int folder) {
    SetSort(s, static_cast<ui::SortColumn>(value), desc ? ui::SortDirection::Desc : ui::SortDirection::Asc);
    // This folder only, like the sort column (Settings keeps the default).
    if (const app::Tab* tab = ActiveTab(s); tab && folder != static_cast<int>(tab->EffectiveFolderSort()))
        SetFolderSort(s, folder);
}

void Kick(AppState& s, HWND hwnd) {
    // The picker is parent-rendered; remove its footprint from the embedded
    // child before the first frame, including while the picker fades closed.
    s.detailsPreview.SetEmbeddedOcclusion(s.renderer.GroupWheelPicker().OcclusionBounds());
    if (s.framePump.Running()) s.framePump.Arm();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Apply(AppState& s, HWND hwnd, int value) {
    auto& wheel = s.renderer.GroupWheelPicker();
    wheel.Close();
    if (g_kind == WheelKind::Sort) ApplySort(s, value, wheel.Desc(), wheel.Folder());
    else SetGroupBy(s, value);
    Kick(s, hwnd);
}

int g_wheel_accum = 0;

} // namespace

bool ApplySortToAllFolders(AppState& s, int column, bool desc, int folder, bool confirm) {
    auto usable = [](const app::Tab* tab) {
        return tab && !tab->content_results && !tab->search_content_active &&
            !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
    };
    const auto option = std::find_if(std::begin(kSortColumns), std::end(kSortColumns),
        [column](const auto& item) { return static_cast<int>(item.column) == column &&
            item.column != ui::SortColumn::Path; });
    if (!usable(ActiveTab(s)) || option == std::end(kSortColumns) || folder < 0 || folder > 2) return false;
    const std::wstring source = ActiveTab(s)->current_path;
    if (confirm) {
        ui::ConfirmDialogSpec spec;
        spec.title = l10n::Get(l10n::StringId::ApplyViewAllTitle);
        spec.message = l10n::Get(l10n::StringId::ApplySortAllMessage);
        spec.confirm_text = l10n::Get(l10n::StringId::ApplyViewAllConfirm);
        spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
        if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return false;
        if (!usable(ActiveTab(s)) || ActiveTab(s)->current_path != source) return false;
    }
    const auto sort_column = static_cast<ui::SortColumn>(column);
    const auto direction = desc ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    s.appPrefs.folder_sorts.ApplyToAll({sort_column, direction});
    s.appPrefs.folder_sort_mode = folder;
    app::SetFolderSortMode(app::FolderSortModeFromInt(folder));
    s.appPrefs.Save();
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        std::vector<std::wstring> paths;
        ForEachPane(window, [&](app::Pane& pane) {
            auto* tab = pane.ActiveTab();
            if (!usable(tab)) return;
            tab->sort_column = sort_column;
            tab->sort_direction = direction;
            tab->folder_sort = -1;
            tab->search_relevance = false;
            if (std::find(paths.begin(), paths.end(), tab->current_path) == paths.end())
                paths.push_back(tab->current_path);
        });
        for (const auto& path : paths) {
            window.store.MarkDirty(path);
            RefreshPath(window, path, RefreshReason::Explicit);
        }
        InvalidateRect(window.hwnd, nullptr, FALSE);
    });
    return true;
}

void OpenGroupWheel(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    const bool recent = kind == L"recent";
    const bool multi = kind == L"search" || kind == L"saved-search" || kind == L"tag" ||
                       kind == L"tags" || kind == L"recycle";
    // Preview only: counting stops at 20k entries so opening stays instant.
    const size_t count = std::min<size_t>(tab->EntryCount(), 20000);
    std::vector<fs::DirEntry> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i) entries.push_back(tab->EntryAt(i));
    ui::GroupWheelData data;
    // Same availability as the menu: Tag needs one parent folder, Location spans folders.
    for (const auto& mode : kModes) {
        if (mode.value == 5 && (multi || recent)) continue;
        if (mode.value == 6 && !multi) continue;
        if (tab->group_by == mode.value) data.applied = static_cast<int>(data.options.size());
        data.options.push_back(BuildOption(*tab, mode, entries));
    }
    if (!multi && !recent) {
        const int media_kind = app::MediaFolderKindFor(*tab);
        for (const auto& extra : kMediaModes) {
            if (extra.kind != media_kind) continue;
            if (tab->group_by == extra.style.value) data.applied = static_cast<int>(data.options.size());
            data.options.push_back(BuildOption(*tab, extra.style, entries));
            data.options.back().apply_all = false;   // session only: never every folder's default
        }
    }
    data.current_text = l10n::Get(l10n::StringId::GroupWheelCurrent);
    data.apply_text = l10n::Get(l10n::StringId::GroupWheelApply);
    data.click_apply_text = l10n::Get(l10n::StringId::GroupWheelClickApply);
    data.cancel_text = l10n::Get(l10n::StringId::Cancel);
    if (!multi && !recent && !tab->content_results && !tab->current_path.empty() &&
        !fs::IsVirtualPath(tab->current_path))
        data.apply_all_text = l10n::Get(l10n::StringId::ApplyGroupAllShort);

    const float width = static_cast<float>(s.compositor.Width());
    const float height = static_cast<float>(s.compositor.Height());
    const float left = s.renderer.EffectiveSidebarWidth(width);
    const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale),
                                                   s.pane ? s.pane->filter_expand : 0.0f);
    g_wheel_accum = 0;
    g_kind = WheelKind::Group;
    s.renderer.GroupWheelPicker().Open(std::move(data), layout.group, D2D1::RectF(0, 0, width, height),
                                       s.scale, ui::motion::SystemAnimationsEnabled());
    Kick(s, s.hwnd);
}

void OpenSortWheel(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    if (kind == L"starred" || kind == L"recent") return;
    const bool show_path = kind == L"search" || kind == L"saved-search" || kind == L"recycle";
    const bool indexed = (kind == L"search" || kind == L"saved-search") && !tab->content_results;
    const std::vector<fs::DirEntry> entries = SampleEntries(show_path);

    ui::GroupWheelData data;
    data.sort_controls = true;
    data.desc = tab->sort_direction == ui::SortDirection::Desc;
    data.folder = std::clamp(static_cast<int>(tab->EffectiveFolderSort()), 0, 2);
    const std::wstring az = L"A\x2192" L"Z", za = L"Z\x2192" L"A";  // split: \x is greedy
    for (const auto& col : kSortColumns) {
        if (col.column == ui::SortColumn::Path && !show_path) continue;
        if (indexed && (col.column == ui::SortColumn::Type || col.column == ui::SortColumn::Path)) continue;
        // Same rule as the sort menu: no creation / access times in search or the recycle bin.
        if (show_path && (col.column == ui::SortColumn::Created || col.column == ui::SortColumn::Accessed)) continue;
        ui::GroupWheelOption o;
        o.value = static_cast<int>(col.column);
        o.icon = static_cast<int>(col.icon);
        o.name = l10n::Get(col.name);
        o.hue1 = col.hue1;
        o.hue2 = col.hue2;
        if (col.column == ui::SortColumn::Mtime || col.column == ui::SortColumn::Created ||
            col.column == ui::SortColumn::Accessed)
            o.directions = {l10n::Get(l10n::StringId::SortOldNew), l10n::Get(l10n::StringId::SortNewOld)};
        else if (col.column == ui::SortColumn::Size)
            o.directions = {l10n::Get(l10n::StringId::SortSmallLarge), l10n::Get(l10n::StringId::SortLargeSmall)};
        else
            o.directions = {az, za};
        std::vector<size_t> order(entries.size());
        for (int d = 0; d < 2; ++d) {
            for (int f = 0; f < 3; ++f) {
                std::iota(order.begin(), order.end(), size_t{0});
                const size_t top = std::min<size_t>(order.size(), 8);
                const auto dir = d ? ui::SortDirection::Desc : ui::SortDirection::Asc;
                const auto mode = app::FolderSortModeFromInt(f);
                std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                                  [&](size_t a, size_t b) {
                                      return app::EntryLess(entries[a], entries[b], col.column, dir, mode);
                                  });
                auto& rows = o.files[static_cast<size_t>(d * 3 + f)];
                for (size_t i = 0; i < top; ++i) {
                    const auto& e = entries[order[i]];
                    rows.push_back({e.name, SortValue(e, col.column), Swatch(e)});
                }
            }
        }
        o.meta = l10n::Get(col.hint);
        if (tab->sort_column == col.column) data.applied = static_cast<int>(data.options.size());
        data.options.push_back(std::move(o));
    }
    data.current_text = l10n::Get(l10n::StringId::GroupWheelCurrent);
    data.apply_text = l10n::Get(l10n::StringId::GroupWheelApply);
    data.click_apply_text = l10n::Get(l10n::StringId::GroupWheelClickApply);
    data.cancel_text = l10n::Get(l10n::StringId::Cancel);
    data.direction_text = l10n::Get(l10n::StringId::SortWheelDirection);
    data.folder_text = l10n::Get(l10n::StringId::SortWheelFolders);
    if (!tab->content_results && !tab->search_content_active && !tab->current_path.empty() &&
        !fs::IsVirtualPath(tab->current_path))
        data.apply_all_text = l10n::Get(l10n::StringId::ApplyGroupAllShort);
    data.folder_names = {l10n::Get(l10n::StringId::SortWheelFolderTop), l10n::Get(l10n::StringId::SortWheelFolderFollow),
                         l10n::Get(l10n::StringId::SortWheelFolderMixed)};

    const float width = static_cast<float>(s.compositor.Width());
    const float height = static_cast<float>(s.compositor.Height());
    const float left = s.renderer.EffectiveSidebarWidth(width);
    const auto layout = s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width - left < 600 * s.scale),
                                                   s.pane ? s.pane->filter_expand : 0.0f);
    g_wheel_accum = 0;
    g_kind = WheelKind::Sort;
    s.renderer.GroupWheelPicker().Open(std::move(data), layout.sort, D2D1::RectF(0, 0, width, height),
                                       s.scale, ui::motion::SystemAnimationsEnabled());
    Kick(s, s.hwnd);
}

// A click that closes the picker can be the first half of a double click: the
// second half must not reach the list below (it would open an item).
static ULONGLONG g_swallow_dblclk_until = 0;

static void SwallowNextDoubleClick() {
    g_swallow_dblclk_until = GetTickCount64() + GetDoubleClickTime();
}

bool GroupWheelMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& wheel = s.renderer.GroupWheelPicker();
    if (msg == WM_LBUTTONDBLCLK && g_swallow_dblclk_until != 0) {
        const bool swallow = GetTickCount64() < g_swallow_dblclk_until;
        g_swallow_dblclk_until = 0;
        if (swallow && !wheel.IsOpen()) return true;
    }
    if (!wheel.IsOpen()) return false;
    const float x = static_cast<float>(GET_X_LPARAM(lParam));
    const float y = static_cast<float>(GET_Y_LPARAM(lParam));
    switch (msg) {
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_UP: wheel.Step(-1); break;
        case VK_DOWN: wheel.Step(1); break;
        case VK_HOME: wheel.StepTo(0); break;
        case VK_END: wheel.StepTo(1000); break;
        case VK_LEFT: wheel.SetDesc(false); break;     // sort picker direction
        case VK_RIGHT: wheel.SetDesc(true); break;
        case VK_RETURN:
        case VK_SPACE: Apply(s, hwnd, wheel.SelectedValue()); return true;
        case VK_ESCAPE: wheel.Close(); break;
        default: break;
        }
        Kick(s, hwnd);
        return true;
    case WM_KEYUP:
    case WM_CHAR:
        return true;
    case WM_SYSKEYDOWN:
        wheel.Close();
        Kick(s, hwnd);
        return false;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!wheel.Contains(x, y)) {          // outside: dismiss, click swallowed
            wheel.Close();
            SwallowNextDoubleClick();
            Kick(s, hwnd);
            return true;
        }
        if (const int button = wheel.HitFooter(x, y)) {   // Cancel / Apply: act on release
            wheel.PressFooter(button);
            wheel.Hover(x, y);
            SetCapture(hwnd);
            Kick(s, hwnd);
            return true;
        }
        if (wheel.HitApplyAll(x, y)) {
            const int value = wheel.SelectedValue();
            const bool desc = wheel.Desc();
            const int folder = wheel.Folder();
            SwallowNextDoubleClick();
            wheel.Close();
            Kick(s, hwnd);
            if (g_kind == WheelKind::Sort) ApplySortToAllFolders(s, value, desc, folder);
            else ApplyGroupToAllFolders(s, value);
            Kick(s, hwnd);
            return true;
        }
        // Double click on any row applies it: the first click already rolled it to the centre.
        if (msg == WM_LBUTTONDBLCLK && wheel.InDrum(x, y)) {
            SwallowNextDoubleClick();
            Apply(s, hwnd, wheel.SelectedValue());
            Kick(s, hwnd);
            return true;
        }
        if (wheel.PointerDown(x, y)) SetCapture(hwnd);
        wheel.Hover(x, y);
        Kick(s, hwnd);
        return true;
    case WM_MOUSEMOVE:
        if (wheel.FooterPressed()) {
            if (wheel.Hover(x, y)) Kick(s, hwnd);
            return true;
        }
        if (wheel.Dragging()) {
            wheel.PointerMove(y);
            Kick(s, hwnd);
            return true;
        }
        if (wheel.Hover(x, y)) Kick(s, hwnd);
        return wheel.Contains(x, y);
    case WM_LBUTTONUP:
        if (const int pressed = wheel.FooterPressed()) {
            ReleaseCapture();
            wheel.PressFooter(0);
            if (wheel.HitFooter(x, y) == pressed) {
                SwallowNextDoubleClick();
                if (pressed == ui::GroupWheel::kFooterApply) Apply(s, hwnd, wheel.SelectedValue());
                else wheel.Close();
            }
            Kick(s, hwnd);
            return true;
        }
        if (wheel.Dragging()) {
            ReleaseCapture();
            const int value = wheel.PointerUp(y);
            if (value >= 0) {
                SwallowNextDoubleClick();
                Apply(s, hwnd, value);
            } else {
                wheel.Hover(x, y);
            }
            Kick(s, hwnd);
            return true;
        }
        return wheel.Contains(x, y);
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        if (wheel.Contains(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
            g_wheel_accum += GET_WHEEL_DELTA_WPARAM(wParam);
            while (g_wheel_accum >= WHEEL_DELTA) { wheel.Step(-1); g_wheel_accum -= WHEEL_DELTA; }
            while (g_wheel_accum <= -WHEEL_DELTA) { wheel.Step(1); g_wheel_accum += WHEEL_DELTA; }
            Kick(s, hwnd);
        }
        return true;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        wheel.Close();
        Kick(s, hwnd);
        return true;
    case WM_KILLFOCUS:
    case WM_SIZE:
    case WM_CANCELMODE:
        wheel.Close();
        Kick(s, hwnd);
        return false;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            wheel.Close();
            Kick(s, hwnd);
        }
        return false;
    default:
        return false;
    }
}

} // namespace pulse
