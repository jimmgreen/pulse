#include "favorites_bar.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "quick_access.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "keymap.h"
#include "../ui/fluent_menu.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <unordered_map>

namespace pulse {
namespace {
constexpr int kFavoriteMenuBase = 36000;   // » and ★⌄ menus: kFavoriteMenuBase + favorite index
constexpr int kIconOnlyCommand = 36900, kHideCommand = 36901, kShowCommand = 36902, kStarHereCommand = 36903;
constexpr int kOpenAllCommand = 36904, kSeparatorCommand = 36905, kUngroupCommand = 36906;
constexpr int kSubfolderBase = 37000;      // ⌄ menu: kSubfolderBase + subfolder index
constexpr size_t kSubfolderMax = 400;
constexpr wchar_t kFolderGlyph[] = L"\xE8B7";
constexpr wchar_t kFileGlyph[] = L"\xE8A5";
constexpr wchar_t kGroupGlyph[] = L"\xE8F1";

// A press on a favorite: a click on release, or a drag along the bar (the
// toolbar pins' model, MoveToolbarPinDrag): the favorite follows the pointer
// 1:1 and takes the slot whose centre is nearest; nothing is saved before the
// release. Below the row it fades; let go there and it leaves the list.
// A group drags as one; a folder let go over the middle of a group joins it.
struct FavoriteDrag {
    const AppState* owner = nullptr;
    bool down = false, active = false, out = false;
    size_t item = SIZE_MAX;        // the dragged favorite (index in starred_items)...
    std::wstring group;            // ...or the dragged group
    int slot = -1;                 // where the bar draws it now (an entry index)
    POINT start{};
    POINT below{};                 // under the entry (screen), for a group's dropdown
    float grab = 0.0f;             // pointer x - its left edge at the press
    float left = 0.0f, dy = 0.0f;
    std::wstring into;             // the group it would join, letting go now
    int into_slot = -1;
    std::vector<size_t> order;     // the bar's order while dragging (starred indices)
};
FavoriteDrag g_drag;

// The entry holding the keyboard focus (F6); -1 none. favorites_bar.size()
// is ».
struct KeyFocus {
    const AppState* owner = nullptr;
    int slot = -1;
};
KeyFocus g_key;

// The favorite last dragged off the bar, for the snack's undo.
struct RemovedFavorite {
    const AppState* owner = nullptr;
    app::StarredItem item;
    size_t index = 0;
};
RemovedFavorite g_removed;

// Offline dimming (§8 不可用): whether each favorite can be reached, asked on a
// worker thread (a dead share can take the whole network timeout) and cached
// for 30 s. The bar only ever reads the cache; an unchecked path counts as
// reachable.
class Reachability {
public:
    bool Offline(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = state_.find(Key(path));
        return it != state_.end() && it->second.offline;
    }
    // Rechecks the stale ones in the background; repaints `hwnd` when an
    // answer changed. At most one worker at a time.
    void Refresh(const std::vector<std::wstring>& paths, HWND hwnd) {
        const ULONGLONG now = GetTickCount64();
        std::vector<std::wstring> due;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (busy_ || now - polled_ < 2000) return;
            polled_ = now;
            for (const auto& path : paths) {
                const Entry& e = state_[Key(path)];
                if (!e.checked || now - e.checked >= 30000) due.push_back(path);
            }
            if (due.empty()) return;
            busy_ = true;
        }
        try {
            std::thread([this, due = std::move(due), hwnd] { Check(due, hwnd); }).detach();
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_);
            busy_ = false;
        }
    }

private:
    struct Entry {
        bool offline = false;
        ULONGLONG checked = 0;
    };
    static std::wstring Key(const std::wstring& path) {
        std::wstring key = path;
        for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
        return key;
    }
    void Check(const std::vector<std::wstring>& due, HWND hwnd) {
        bool changed = false;
        for (const auto& path : due) {
            const bool offline = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
            std::lock_guard<std::mutex> lock(m_);
            Entry& e = state_[Key(path)];
            changed = changed || e.offline != offline;
            e.offline = offline;
            e.checked = GetTickCount64();
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            busy_ = false;
        }
        if (changed && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
    }
    std::mutex m_;
    std::unordered_map<std::wstring, Entry> state_;
    ULONGLONG polled_ = 0;
    bool busy_ = false;
};

// Never destroyed: a worker may still be asking a share at exit.
Reachability& Reach() {
    static Reachability* reach = new Reachability;
    return *reach;
}

bool DraggingIn(const AppState& s) {
    return g_drag.down && g_drag.owner == &s && g_drag.order.size() == s.places.starred_items.size();
}

// The bar's entries (app::StarredRow: favorites, groups, separators) with
// the list in `order` (null: as saved); indices stay into starred_items.
std::vector<app::StarredRow> RowsFor(const AppState& s, const std::vector<size_t>* order) {
    const auto& list = s.places.starred_items;
    if (!order) return app::MakeStarredRows(list, s.places.starred_groups, false);
    std::vector<app::StarredItem> items;
    items.reserve(order->size());
    for (size_t index : *order) items.push_back(list[index]);
    auto rows = app::MakeStarredRows(items, s.places.starred_groups, false);
    for (auto& row : rows) {
        row.index = (*order)[row.index];
        for (auto& member : row.members) member = (*order)[member];
    }
    return rows;
}

std::vector<app::StarredRow> BarRows(const AppState& s) {
    return RowsFor(s, DraggingIn(s) ? &g_drag.order : nullptr);
}

std::optional<app::StarredRow> RowAt(const AppState& s, int slot) {
    if (slot < 0) return std::nullopt;
    auto rows = BarRows(s);
    if (static_cast<size_t>(slot) >= rows.size()) return std::nullopt;
    return std::move(rows[static_cast<size_t>(slot)]);
}

// The favorite the bar shows at `slot` (null for a group or a separator).
const app::StarredItem* FavoriteAt(const AppState& s, int slot) {
    const auto row = RowAt(s, slot);
    if (!row || row->kind != app::StarredRow::Kind::Item || row->index >= s.places.starred_items.size())
        return nullptr;
    return &s.places.starred_items[row->index];
}

// The group the bar shows at `slot` ("" for anything else).
std::wstring GroupAt(const AppState& s, int slot) {
    const auto row = RowAt(s, slot);
    return row && row->kind == app::StarredRow::Kind::Group ? row->group : std::wstring{};
}

int SlotOf(const std::vector<app::StarredRow>& rows, size_t item, const std::wstring& group) {
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if ((row.kind == app::StarredRow::Kind::Item && row.index == item) ||
            (row.kind == app::StarredRow::Kind::Group && !group.empty() && row.group == group))
            return static_cast<int>(i);
    }
    return -1;
}

// A group's members, in list order.
std::vector<std::wstring> GroupPaths(const AppState& s, const std::wstring& id) {
    std::vector<std::wstring> paths;
    for (const auto& item : s.places.starred_items)
        if (!id.empty() && item.group == id && item.kind != app::PlaceItemKind::File) paths.push_back(item.path);
    return paths;
}

bool IsFolder(const app::StarredItem& item) { return item.kind != app::PlaceItemKind::File; }

void OpenGroupTabs(AppState& s, const std::wstring& id) {
    for (const auto& path : GroupPaths(s, id)) OpenFolderTab(s, path);
}

// how: 0 here, 1 a new tab, 2 the other pane (a new tab without one).
void OpenFolder(AppState& s, const std::wstring& path, int how) {
    if (how == 2) {
        app::Pane* other = s.targetPane && s.targetPane != s.pane ? s.targetPane : nullptr;
        if (!other)
            for (app::Pane* pane : LiveLayout(s).VisiblePanes())
                if (pane && pane != s.pane) { other = pane; break; }
        if (other) {
            FocusPane(s, other);
            NavigateTo(s, path);
            return;
        }
        how = 1;
    }
    if (how == 1) OpenFolderTab(s, path);
    else NavigateTo(s, path);
}

void OpenFavorite(AppState& s, const app::StarredItem& item, int how) {
    if (item.kind == app::PlaceItemKind::File) {
        OpenPath(s, item.path);   // the default program
        return;
    }
    OpenFolder(s, item.path, how);
}

int HowFromKeys() {
    if (GetKeyState(VK_SHIFT) & 0x8000) return 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 1;
    return 0;
}

void SaveAndRepaint(AppState& s) {
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::wstring ChildOf(const std::wstring& dir, const std::wstring& name) {
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + name;
}

// Folders directly inside `dir`, sorted as Explorer does (digits as
// numbers); hidden and system ones are left out. False: it cannot be listed.
bool ListSubfolders(const std::wstring& dir, std::vector<std::wstring>& names) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(ChildOf(dir, L"*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
            std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0)
            continue;
        names.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                               a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return true;
}

// Lists on a worker thread: a share that has not answered in 800 ms counts
// as unreachable, so the UI never waits on the network for longer (the
// worker finishes on its own).
bool ListSubfoldersBounded(const std::wstring& dir, std::vector<std::wstring>& names) {
    struct Listing {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, ok = false;
        std::vector<std::wstring> names;
    };
    auto job = std::make_shared<Listing>();
    try {
        std::thread([job, dir] {
            std::vector<std::wstring> found;
            const bool ok = ListSubfolders(dir, found);
            std::lock_guard<std::mutex> lock(job->m);
            job->ok = ok;
            job->names = std::move(found);
            job->done = true;
            job->cv.notify_all();
        }).detach();
    } catch (...) {
        return false;
    }
    std::unique_lock<std::mutex> lock(job->m);
    if (!job->cv.wait_for(lock, std::chrono::milliseconds(800), [&] { return job->done; })) return false;
    names = std::move(job->names);
    return job->ok;
}

ui::FluentMenuItem DisabledItem(l10n::StringId id) {
    ui::FluentMenuItem item;
    item.text = l10n::Get(id);
    item.enabled = false;
    return item;
}

// ⌄ on a favorite folder: its subfolders; one opens (Ctrl: a new tab,
// Shift: the other pane).
void ShowSubfolderMenu(AppState& s, const std::wstring& folder, POINT screen) {
    if (!EnsureMenu(s)) return;
    std::vector<std::wstring> names;
    const bool ok = ListSubfoldersBounded(folder, names);
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < names.size() && i < kSubfolderMax; ++i) {
        ui::FluentMenuItem item;
        item.command = kSubfolderBase + static_cast<int>(i);
        item.text = names[i];
        item.glyph = kFolderGlyph;
        items.push_back(std::move(item));
    }
    if (items.empty())
        items.push_back(DisabledItem(ok ? l10n::StringId::FavoritesBarNoSubfolders
                                        : l10n::StringId::FavoritesBarUnreachable));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    const int i = cmd - kSubfolderBase;
    if (cmd < kSubfolderBase || i < 0 || static_cast<size_t>(i) >= names.size()) return;
    OpenFolder(s, ChildOf(folder, names[static_cast<size_t>(i)]), HowFromKeys());
}

// Right-click on the bar's empty part: icons only, hide.
void ShowFavoritesBarMenu(AppState& s, POINT screen) {
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem icons;
    icons.command = kIconOnlyCommand;
    icons.text = l10n::Get(l10n::StringId::FavoritesBarIconOnly);
    icons.checked = s.appPrefs.favorites_bar_icon_only;
    items.push_back(std::move(icons));
    ui::FluentMenuItem hide;
    hide.command = kHideCommand;
    hide.text = l10n::Get(l10n::StringId::FavoritesBarHide);
    hide.glyph = L"\xED1A";   // hide
    hide.shortcut = app::ShownShortcut(L"Ctrl+Shift+B");
    items.push_back(std::move(hide));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == kIconOnlyCommand) {
        s.appPrefs.favorites_bar_icon_only = !s.appPrefs.favorites_bar_icon_only;
        SaveAndRepaint(s);
    } else if (cmd == kHideCommand) {
        ToggleFavoritesBar(s);
    }
}

ui::FluentMenuItem FavoriteMenuItem(const app::StarredItem& item, size_t index) {
    ui::FluentMenuItem out;
    out.command = kFavoriteMenuBase + static_cast<int>(index);
    out.text = app::TabTitle(item.path);
    out.glyph = item.kind == app::PlaceItemKind::File ? kFileGlyph : kFolderGlyph;
    return out;
}

void OpenFavoriteCommand(AppState& s, int cmd) {
    if (cmd < kFavoriteMenuBase) return;
    const size_t index = static_cast<size_t>(cmd - kFavoriteMenuBase);
    if (index < s.places.starred_items.size())
        OpenFavorite(s, app::StarredItem(s.places.starred_items[index]), HowFromKeys());
}

// The bar's entries from `first` as menu rows: a group is a flyout of its
// members, a separator a menu separator.
void AppendFavoriteRows(const AppState& s, std::vector<ui::FluentMenuItem>& items, size_t first) {
    const auto& list = s.places.starred_items;
    const auto rows = BarRows(s);
    for (size_t r = first; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.kind == app::StarredRow::Kind::Separator) {
            if (!items.empty()) items.back().separator_after = true;
            continue;
        }
        if (row.kind == app::StarredRow::Kind::Group) {
            const app::StarredGroup* group = s.places.FindStarredGroup(row.group);
            ui::FluentMenuItem flyout;
            flyout.text = group ? group->name : std::wstring{};
            flyout.glyph = kGroupGlyph;
            for (size_t member : row.members) flyout.children.push_back(FavoriteMenuItem(list[member], member));
            if (!flyout.children.empty()) items.push_back(std::move(flyout));
            continue;
        }
        if (row.index < list.size()) items.push_back(FavoriteMenuItem(list[row.index], row.index));
    }
}

// »: the favorites without room on the bar, in list order.
void ShowFavoritesOverflow(AppState& s, int first, POINT screen) {
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    AppendFavoriteRows(s, items, static_cast<size_t>(std::max(first, 0)));
    if (items.empty()) return;
    OpenFavoriteCommand(s, s.menu->TrackPopup(screen, std::move(items)));
}

// A group's button: its members, then 全部在新标签页打开.
void ShowGroupDropdown(AppState& s, const std::wstring& id, POINT screen) {
    if (!EnsureMenu(s)) return;
    const auto& list = s.places.starred_items;
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].group == id && IsFolder(list[i])) items.push_back(FavoriteMenuItem(list[i], i));
    if (items.empty()) return;
    items.back().separator_after = true;
    ui::FluentMenuItem all;
    all.command = kOpenAllCommand;
    all.text = l10n::Get(l10n::StringId::FavoritesGroupOpenAll);
    all.glyph = L"\xE8A7";   // open in new window
    items.push_back(std::move(all));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == kOpenAllCommand) OpenGroupTabs(s, id);
    else OpenFavoriteCommand(s, cmd);
}

// The address bar's ★⌄ (the bar hidden): star / unstar this folder, then the
// whole list, then a way to show the bar.
void ShowFavoritesDropdown(AppState& s, POINT screen) {
    if (!EnsureMenu(s)) return;
    const app::Tab* tab = ActiveTab(s);
    const std::wstring here = tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)
        ? tab->current_path : std::wstring{};
    const bool starred = !here.empty() && s.places.IsStarred(here);
    std::vector<ui::FluentMenuItem> items;
    if (!here.empty()) {
        ui::FluentMenuItem star;
        star.command = kStarHereCommand;
        star.text = l10n::Get(starred ? l10n::StringId::FavoritesUnstarHere : l10n::StringId::FavoritesStarHere);
        star.glyph = starred ? L"\xE735" : L"\xE734";
        star.separator_after = true;
        items.push_back(std::move(star));
    }
    AppendFavoriteRows(s, items, 0);
    if (s.places.starred_items.empty()) items.push_back(DisabledItem(l10n::StringId::NoStarred));
    items.back().separator_after = true;
    ui::FluentMenuItem show;
    show.command = kShowCommand;
    show.text = l10n::Get(l10n::StringId::FavoritesBarShow);
    show.shortcut = app::ShownShortcut(L"Ctrl+Shift+B");
    items.push_back(std::move(show));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == kStarHereCommand) ToggleStarred(s, here, app::PlaceItemKind::Folder);
    else if (cmd == kShowCommand) ToggleFavoritesBar(s);
    else OpenFavoriteCommand(s, cmd);
}

POINT BelowControl(const AppState& s, const ui::HitTestResult& hit) {
    POINT pt{static_cast<LONG>(std::lround(hit.control_bounds.left)),
             static_cast<LONG>(std::lround(hit.control_bounds.bottom))};
    ClientToScreen(s.hwnd, &pt);
    return pt;
}

void BeginDrag(AppState& s, const ui::HitTestResult& hit) {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    const auto row = RowAt(s, hit.index);
    if (!row || row->kind == app::StarredRow::Kind::Separator) return;
    g_drag = FavoriteDrag{};
    g_drag.owner = &s;
    g_drag.down = true;
    g_drag.slot = hit.index;
    if (row->kind == app::StarredRow::Kind::Group) g_drag.group = row->group;
    else g_drag.item = row->index;
    g_drag.start = pt;
    g_drag.below = BelowControl(s, hit);
    g_drag.grab = static_cast<float>(pt.x) - hit.control_bounds.left;
    g_drag.order.resize(s.places.starred_items.size());
    std::iota(g_drag.order.begin(), g_drag.order.end(), size_t{0});
    SetCapture(s.hwnd);
}

// Off the list, with the snack's undo.
void RemoveFavorite(AppState& s, size_t index) {
    if (index >= s.places.starred_items.size()) return;
    g_removed = RemovedFavorite{&s, s.places.starred_items[index], index};
    const std::wstring path = g_removed.item.path;
    ToggleStarred(s, path);   // starred, so this removes it
    if (!s.hwnd) return;
    s.notification_toast.ShowSnack(
        s.hwnd, ui::SnackIcon::Undo, l10n::Get(l10n::StringId::FavoritesBarRemoved), app::TabTitle(path),
        {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_FAVORITE_UNDO}});
}
} // namespace

void FillFavoritesBar(AppState& s, ui::WindowViewModel& vm) {
    const bool on = s.appPrefs.favorites_bar && !s.renderer.Embedded();
    s.renderer.SetFavoritesBar(on);
    vm.favorites_bar_on = on;
    const app::Tab* tab = ActiveTab(s);
    if (!on) {
        // The address bar's ★⌄ shows whether this folder is starred.
        vm.favorites_here = tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
                            s.places.IsStarred(tab->current_path);
        // The sidebar's 收藏夹 row offers the bar (an embedded window has none).
        if (!s.renderer.Embedded()) {
            const std::wstring root = app::MakeStarredPath();
            for (auto& group : vm.sidebar)
                for (auto& row : group.items)
                    if (row.indent == 0 && row.path == root) row.bar_switch = true;
        }
        return;
    }
    vm.favorites_bar_icon_only = s.appPrefs.favorites_bar_icon_only;
    vm.favorites_bar_hint = l10n::Get(l10n::StringId::FavoritesBarEmpty);
    vm.favorites_bar_more = l10n::Get(l10n::StringId::FavoritesBarMore);
    vm.favorites_bar_drop = s.dropFavorite;
    vm.favorites_bar_drop_add = s.dropFavoriteAdd;
    // Starred paths are normalized (\\?\C:\...); compare as display text.
    const std::wstring here = tab ? pulse::path::FriendlyPathText(tab->current_path) : std::wstring{};
    const auto is_here = [&](const std::wstring& path) {
        return !here.empty() && _wcsicmp(here.c_str(), pulse::path::FriendlyPathText(path).c_str()) == 0;
    };
    const auto& list = s.places.starred_items;
    {
        std::vector<std::wstring> paths;
        paths.reserve(list.size());
        for (const auto& entry : list) paths.push_back(entry.path);
        Reach().Refresh(paths, s.hwnd);
    }
    const auto rows = BarRows(s);
    vm.favorites_bar.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.index >= list.size()) continue;
        ui::WindowViewModel::FavoritesBarItem item;
        if (row.kind == app::StarredRow::Kind::Separator) {
            item.separator = true;
            item.path = L"pulse:star-sep:" + list[row.index].path;   // keys its motion
        } else if (row.kind == app::StarredRow::Kind::Group) {
            const app::StarredGroup* group = s.places.FindStarredGroup(row.group);
            item.group = true;
            item.path = app::MakeStarredGroupPath(row.group);
            item.label = group ? group->name : std::wstring{};
            item.count = static_cast<int>(row.members.size());
            for (size_t member : row.members) item.current = item.current || is_here(list[member].path);
        } else {
            const app::StarredItem& entry = list[row.index];
            item.path = entry.path;
            item.label = app::TabTitle(entry.path);
            item.folder = IsFolder(entry);
            item.current = item.folder && is_here(entry.path);
            item.offline = Reach().Offline(entry.path);
        }
        vm.favorites_bar.push_back(std::move(item));
    }
    if (DraggingIn(s) && g_drag.active) {
        vm.favorites_bar_drag = g_drag.slot;
        vm.favorites_bar_drag_left = g_drag.left;
        vm.favorites_bar_drag_dy = g_drag.dy;
        vm.favorites_bar_drag_out = g_drag.out;
        if (!g_drag.into.empty()) vm.favorites_bar_drop = g_drag.into_slot;   // the ring: joins it
    }
    if (g_key.owner == &s && g_key.slot >= 0)
        vm.favorites_bar_focus = std::min(g_key.slot, static_cast<int>(vm.favorites_bar.size()));
}

bool FavoritesBarShown(const AppState& s) {
    return s.appPrefs.favorites_bar && !s.renderer.Embedded();
}

uint32_t SidebarHiddenMaskWithBar(const AppState& s) {
    const uint32_t starred = 1u << static_cast<int>(app::SidebarSectionId::Starred);
    return s.sidebarHiddenMask | (FavoritesBarShown(s) ? starred : 0u);
}

void ToggleFavoritesBar(AppState& s) {
    s.appPrefs.favorites_bar = !s.appPrefs.favorites_bar;
    // Leaving the bar brings the list back vertically in the sidebar.
    if (!s.appPrefs.favorites_bar)
        s.sidebarHiddenMask &= ~(1u << static_cast<int>(app::SidebarSectionId::Starred));
    s.renderer.SetFavoritesBar(s.appPrefs.favorites_bar && !s.renderer.Embedded());
    SaveAndRepaint(s);
}

bool HandleFavoritesBarPress(AppState& s, const ui::HitTestResult& hit) {
    if (g_key.slot >= 0) {   // the mouse takes over from the keyboard
        g_key = KeyFocus{};
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    if ((hit.region == ui::HitTestResult::SidebarItem || hit.region == ui::HitTestResult::SidebarItemExpand) &&
        app::IsStarredGroupPath(hit.path)) {
        // A group's row in the sidebar's 收藏夹 is not a place: it folds.
        const std::wstring id = app::StarredGroupIdFromPath(hit.path);
        if (const app::StarredGroup* group = s.places.FindStarredGroup(id))
            s.places.SetStarredGroupCollapsed(id, !group->collapsed);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (hit.region == ui::HitTestResult::AddressFavorites) {
        ShowFavoritesDropdown(s, BelowControl(s, hit));
        return true;
    }
    // 改成横向收藏栏 / 放回侧边栏: the Ctrl+Shift+B switch, from either side.
    if (hit.region == ui::HitTestResult::SidebarBarSwitch || hit.region == ui::HitTestResult::FavoritesBarDock) {
        ToggleFavoritesBar(s);
        return true;
    }
    if (hit.region == ui::HitTestResult::FavoritesBarMore) {
        ShowFavoritesOverflow(s, hit.index, BelowControl(s, hit));
        return true;
    }
    if (hit.region == ui::HitTestResult::FavoritesBarChevron) {
        if (const app::StarredItem* item = FavoriteAt(s, hit.index)) {
            const std::wstring folder = item->path;
            ShowSubfolderMenu(s, folder, BelowControl(s, hit));
        }
        return true;
    }
    if (hit.region != ui::HitTestResult::FavoritesBarItem) return false;
    BeginDrag(s, hit);   // a favorite or a group; the empty part does nothing
    return true;
}

bool FavoritesBarDragging() { return g_drag.down; }

bool MoveFavoritesBarDrag(AppState& s, POINT pt) {
    FavoriteDrag& d = g_drag;
    if (!d.down) return false;
    if (!DraggingIn(s)) {   // the list changed under the press
        EndFavoritesBarDrag(s, true);
        return false;
    }
    const float dx = static_cast<float>(pt.x - d.start.x), dy = static_cast<float>(pt.y - d.start.y);
    if (!d.active) {
        if (std::hypot(dx, dy) < 4.0f * s.scale) return false;
        d.active = true;
    }
    const ui::WindowViewModel vm = BuildVm(s);
    const ui::FavoritesBarLayout lay = s.renderer.FavoritesBarLayoutAt(vm, static_cast<float>(s.compositor.Width()));
    const auto rows = RowsFor(s, &d.order);
    if (d.slot < 0 || static_cast<size_t>(d.slot) >= lay.items.size() ||
        static_cast<size_t>(d.slot) >= rows.size())
        return true;
    const auto& list = s.places.starred_items;
    if (d.item < list.size()) {
        const auto& item = list[d.item];
        const auto source = item.kind == app::PlaceItemKind::Folder
            ? ShortcutDragSource::FavoriteFolder : ShortcutDragSource::FavoriteFile;
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                     static_cast<float>(s.compositor.Height()));
        const auto hit = s.renderer.HitTest(vm, rect, static_cast<float>(pt.x), static_cast<float>(pt.y));
        if (PreviewShortcutCrossDrop(s, source, item.path, hit, pt)) {
            // A collection is not the bar's "drag down to remove" gesture.
            d.out = false;
            d.into.clear();
            d.into_slot = -1;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return true;
        }
    }
    const auto folder_row = [&](const app::StarredRow& row) {
        return row.kind == app::StarredRow::Kind::Group || IsFolder(list[row.index]);
    };
    const app::StarredRow& self = rows[static_cast<size_t>(d.slot)];
    const bool folder = folder_row(self);
    const D2D1_RECT_F home = lay.items[static_cast<size_t>(d.slot)];
    const float w = home.right - home.left;
    d.left = static_cast<float>(pt.x) - d.grab;
    // Below the row it stays in sight (14 DIP down at most), faded once
    // letting go would take it off. A group does not come off this way.
    d.dy = std::clamp(dy, -4.0f * s.scale, 14.0f * s.scale);
    d.out = self.kind == app::StarredRow::Kind::Item && dy > 34.0f * s.scale;
    // Over the middle half of a group, a folder joins it (the group's ring).
    d.into.clear();
    d.into_slot = -1;
    if (self.kind == app::StarredRow::Kind::Item && folder && !d.out) {
        for (size_t i = 0; i < rows.size() && i < lay.items.size(); ++i) {
            const D2D1_RECT_F r = lay.items[i];
            if (rows[i].kind != app::StarredRow::Kind::Group || r.right <= r.left) continue;
            const float inset = (r.right - r.left) * 0.25f;
            if (static_cast<float>(pt.x) >= r.left + inset && static_cast<float>(pt.x) <= r.right - inset) {
                d.into = rows[i].group;
                d.into_slot = static_cast<int>(i);
                break;
            }
        }
    }
    if (d.into.empty() && !d.out) {
        // The entry whose centre is nearest its own; folders stay ahead of files.
        int best = d.slot;
        float best_dist = 1e9f;
        for (size_t i = 0; i < lay.items.size() && i < rows.size(); ++i) {
            const D2D1_RECT_F r = lay.items[i];
            if (r.right <= r.left || rows[i].kind == app::StarredRow::Kind::Separator ||
                folder_row(rows[i]) != folder)
                continue;
            const float dist = std::fabs((r.left + r.right) * 0.5f - (d.left + w * 0.5f));
            if (dist < best_dist) { best_dist = dist; best = static_cast<int>(i); }
        }
        if (best != d.slot) {
            // Move the entry, then flatten back to the list: a group brings its
            // members, a separator follows its item when the rows are rebuilt.
            std::vector<app::StarredRow> moved = rows;
            app::StarredRow entry = moved[static_cast<size_t>(d.slot)];
            moved.erase(moved.begin() + d.slot);
            moved.insert(moved.begin() + best, std::move(entry));
            std::vector<size_t> order;
            order.reserve(d.order.size());
            for (const auto& row : moved) {
                if (row.kind == app::StarredRow::Kind::Item) order.push_back(row.index);
                else if (row.kind == app::StarredRow::Kind::Group)
                    order.insert(order.end(), row.members.begin(), row.members.end());
            }
            if (order.size() == d.order.size()) {
                d.order = std::move(order);
                const int now = SlotOf(RowsFor(s, &d.order), d.item, d.group);
                if (now >= 0) d.slot = now;
            }
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void EndFavoritesBarDrag(AppState& s, bool cancel, POINT client) {
    const FavoriteDrag d = g_drag;
    if (!d.down) return;
    g_drag = FavoriteDrag{};
    if (d.owner == &s && d.active && d.item != SIZE_MAX) ClearShortcutCrossDropHint(s);
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (d.owner != &s || cancel || d.slot < 0) return;
    const auto& list = s.places.starred_items;
    if (!d.active) {
        if (!d.group.empty()) ShowGroupDropdown(s, d.group, d.below);
        else if (d.item < list.size()) OpenFavorite(s, app::StarredItem(list[d.item]), HowFromKeys());
        return;
    }
    if (d.order.size() != list.size()) return;
    if (d.item < list.size()) {
        const auto& item = list[d.item];
        const auto source = item.kind == app::PlaceItemKind::Folder
            ? ShortcutDragSource::FavoriteFolder : ShortcutDragSource::FavoriteFile;
        if (CommitShortcutCrossDrop(s, source, item.path, HitShortcutCrossDrop(s, client))) return;
    }
    if (d.out) {
        if (d.item < list.size()) RemoveFavorite(s, d.item);
        return;
    }
    const std::wstring path = d.item < list.size() ? list[d.item].path : std::wstring{};
    bool changed = s.places.ReorderStarred(d.order);
    if (!d.into.empty() && !path.empty()) changed = s.places.SetStarredItemGroup(path, d.into) || changed;
    if (changed) RefreshStarredViews(s);
}

void ShowStarredGroupMenu(AppState& s, const std::wstring& id, POINT screen) {
    const app::StarredGroup* group = s.places.FindStarredGroup(id);
    if (!group || !EnsureMenu(s)) return;
    const std::wstring name = group->name;
    // The name is the menu's field (the tab group editor's model): typing
    // renames as it goes; an emptied field keeps the last name.
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::FavoritesGroupName));
    s.menu->SetInitialFilterText(name);
    s.menu->SetFilterMinWidth(260.0f);
    const auto build = [&s, id](const std::wstring& query) {
        if (!query.empty() && s.places.RenameStarredGroup(id, query)) InvalidateRect(s.hwnd, nullptr, FALSE);
        const app::StarredGroup* current = s.places.FindStarredGroup(id);
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem all;
        all.command = kOpenAllCommand;
        all.text = l10n::Get(l10n::StringId::FavoritesGroupOpenAll);
        all.glyph = L"\xE8A7";
        all.separator_after = true;
        items.push_back(std::move(all));
        ui::FluentMenuItem line;
        line.command = kSeparatorCommand;
        line.text = l10n::Get(current && current->separator_after ? l10n::StringId::FavoritesSeparatorRemove
                                                                   : l10n::StringId::FavoritesSeparatorAdd);
        items.push_back(std::move(line));
        ui::FluentMenuItem ungroup;
        ungroup.command = kUngroupCommand;
        ungroup.text = l10n::Get(l10n::StringId::FavoritesGroupUngroup);
        ungroup.glyph = L"\xE8C6";   // cut apart
        items.push_back(std::move(ungroup));
        return items;
    };
    const int cmd = s.menu->TrackPopup(screen, build(name), build);
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    if (cmd == kOpenAllCommand) {
        OpenGroupTabs(s, id);
    } else if (cmd == kSeparatorCommand) {
        if (const app::StarredGroup* current = s.places.FindStarredGroup(id))
            s.places.SetStarredSeparator(id, !current->separator_after);
    } else if (cmd == kUngroupCommand) {
        s.places.UngroupStarred(id);
    }
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CreateStarredGroupAndEdit(AppState& s, const std::wstring& path, POINT screen) {
    const std::wstring id = s.places.CreateStarredGroup(l10n::Get(l10n::StringId::FavoritesGroupDefaultName), path);
    if (id.empty()) return;
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    ShowStarredGroupMenu(s, id, screen);   // name it
}

bool FavoritesBarHasFocus(const AppState& s) {
    return g_key.owner == &s && g_key.slot >= 0 && s.appPrefs.favorites_bar && !s.renderer.Embedded() &&
           !s.places.starred_items.empty();
}

bool CycleFavoritesBarFocus(AppState& s) {
    if (FavoritesBarHasFocus(s)) {
        g_key = KeyFocus{};
        const auto panes = LiveLayout(s).VisiblePanes();
        if (!panes.empty() && panes.front()) FocusPane(s, panes.front());
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (!s.appPrefs.favorites_bar || s.renderer.Embedded() || s.places.starred_items.empty()) return false;
    // After the last pane; before that F6 still goes pane to pane.
    const auto panes = LiveLayout(s).VisiblePanes();
    if (!panes.empty() && s.pane && s.pane != panes.back()) return false;
    g_key = KeyFocus{&s, 0};
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool HandleFavoritesBarKey(AppState& s, WPARAM key) {
    if (!FavoritesBarHasFocus(s)) {
        g_key = KeyFocus{};
        return false;
    }
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) return true;
    if (key == VK_F6) return false;   // the F6 cycle takes it on
    const auto leave = [&] {
        g_key = KeyFocus{};
        InvalidateRect(s.hwnd, nullptr, FALSE);
    };
    if (key == VK_ESCAPE) {
        leave();
        return true;
    }
    const ui::WindowViewModel vm = BuildVm(s);
    const ui::FavoritesBarLayout lay = s.renderer.FavoritesBarLayoutAt(vm, static_cast<float>(s.compositor.Width()));
    // The stops: the entries on the bar (not the separators), then ».
    std::vector<int> stops;
    for (size_t i = 0; i < vm.favorites_bar.size() && i < lay.items.size(); ++i)
        if (!vm.favorites_bar[i].separator && lay.items[i].right > lay.items[i].left) stops.push_back(static_cast<int>(i));
    const int more = static_cast<int>(vm.favorites_bar.size());
    if (lay.first_hidden >= 0) stops.push_back(more);
    if (stops.empty()) {
        leave();
        return false;
    }
    size_t at = 0;
    while (at + 1 < stops.size() && stops[at] < g_key.slot) ++at;
    const int slot = stops[at];
    const auto move_to = [&](size_t index) {
        g_key.slot = stops[std::min(index, stops.size() - 1)];
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    };
    if (!alt && key == VK_LEFT) return move_to(at == 0 ? 0 : at - 1);
    if (!alt && key == VK_RIGHT) return move_to(at + 1);
    if (!alt && key == VK_HOME) return move_to(0);
    if (!alt && key == VK_END) return move_to(stops.size() - 1);
    const D2D1_RECT_F r = slot == more ? lay.more : lay.items[static_cast<size_t>(slot)];
    POINT below{static_cast<LONG>(std::lround(r.left)), static_cast<LONG>(std::lround(r.bottom))};
    ClientToScreen(s.hwnd, &below);
    const app::StarredItem* item = slot == more ? nullptr : FavoriteAt(s, slot);
    const std::wstring group = slot == more ? std::wstring{} : GroupAt(s, slot);
    if (key == VK_APPS || (key == VK_F10 && shift)) {
        if (item) ShowCuratedItemMenu(s, std::wstring(item->path), false, below);
        else if (!group.empty()) ShowStarredGroupMenu(s, group, below);
        else ShowFavoritesBarMenu(s, below);
        return true;
    }
    if (key == VK_DOWN) {
        if (slot == more) ShowFavoritesOverflow(s, lay.first_hidden, below);
        else if (!group.empty()) ShowGroupDropdown(s, group, below);
        else if (item && IsFolder(*item)) ShowSubfolderMenu(s, std::wstring(item->path), below);
        return true;
    }
    if (!alt && (key == VK_RETURN || key == VK_SPACE)) {
        if (slot == more) {
            ShowFavoritesOverflow(s, lay.first_hidden, below);
        } else if (!group.empty()) {
            ShowGroupDropdown(s, group, below);
        } else if (item) {
            const app::StarredItem copy = *item;
            leave();   // like a link: focus goes on to what it opened
            OpenFavorite(s, copy, HowFromKeys());
        }
        return true;
    }
    leave();
    return false;
}

bool OpenFavoritesGroupTabs(AppState& s, const std::wstring& path) {
    if (!app::IsStarredGroupPath(path)) return false;
    OpenGroupTabs(s, app::StarredGroupIdFromPath(path));
    return true;
}

void UndoFavoriteRemoval(AppState& s) {
    RemovedFavorite r = std::move(g_removed);
    g_removed = RemovedFavorite{};
    if (r.owner != &s || r.item.path.empty() || s.places.IsStarred(r.item.path)) return;
    ToggleStarred(s, r.item.path, r.item.kind);
    // It comes back at the end of its kind; take it back to its place.
    const auto& list = s.places.starred_items;
    for (size_t i = 0; i < list.size(); ++i) {
        if (_wcsicmp(list[i].path.c_str(), r.item.path.c_str()) != 0) continue;
        s.places.MoveStarredItem(i, std::min(r.index, list.size() - 1));
        break;
    }
    if (!r.item.badge.empty() || r.item.badge_rgb != app::kDefaultBadgeRgb)
        s.places.SetStarredBadge(r.item.path, r.item.badge, r.item.badge_rgb);
    // Its group is kept in memory while empty, so it goes back in.
    if (!r.item.group.empty()) s.places.SetStarredItemGroup(r.item.path, r.item.group, false);
    if (r.item.separator_after) s.places.SetStarredSeparator(r.item.path, true);
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleFavoritesBarContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen) {
    if (hit.region == ui::HitTestResult::SidebarItem && app::IsStarredGroupPath(hit.path)) {
        ShowStarredGroupMenu(s, app::StarredGroupIdFromPath(hit.path), screen);
        return true;
    }
    if (hit.region == ui::HitTestResult::AddressFavorites) {
        ShowFavoritesDropdown(s, screen);
        return true;
    }
    if (hit.region != ui::HitTestResult::FavoritesBarItem && hit.region != ui::HitTestResult::FavoritesBarMore &&
        hit.region != ui::HitTestResult::FavoritesBarChevron)
        return false;
    if (hit.region != ui::HitTestResult::FavoritesBarMore) {
        // A favorite: the same menu as its row in the sidebar's 收藏夹.
        if (const app::StarredItem* item = FavoriteAt(s, hit.index)) {
            ShowCuratedItemMenu(s, std::wstring(item->path), false, screen);
            return true;
        }
        if (const std::wstring group = GroupAt(s, hit.index); !group.empty()) {
            ShowStarredGroupMenu(s, group, screen);
            return true;
        }
    }
    ShowFavoritesBarMenu(s, screen);
    return true;
}

std::wstring FavoritesBarMiddleClickPath(const AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::FavoritesBarItem && hit.region != ui::HitTestResult::FavoritesBarChevron)
        return {};
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    return item && IsFolder(*item) ? item->path : std::wstring{};
}

std::wstring FavoritesBarDropFolder(const AppState& s, const ui::HitTestResult& hit, bool* offline) {
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    const bool away = item && Reach().Offline(item->path);
    if (offline) *offline = away;
    return item && IsFolder(*item) && !away ? item->path : std::wstring{};
}

std::wstring FavoritesBarGroupAt(const AppState& s, const ui::HitTestResult& hit) {
    return GroupAt(s, hit.index);
}

namespace {
std::wstring g_drop_group;
}

void SetFavoritesBarDropGroup(std::wstring id) { g_drop_group = std::move(id); }

const std::wstring& FavoritesBarDropGroup() { return g_drop_group; }

std::wstring FavoritesBarTooltip(const AppState& s, const ui::HitTestResult& hit) {
    if (const std::wstring id = GroupAt(s, hit.index); !id.empty()) {
        // A group: its name, then its members.
        const app::StarredGroup* group = s.places.FindStarredGroup(id);
        std::wstring text = group ? group->name : std::wstring{};
        const auto paths = GroupPaths(s, id);
        for (size_t i = 0; i < paths.size() && i < 8; ++i) text += L"\n" + app::TabTitle(paths[i]);
        if (paths.size() > 8) text += L"\n\u2026";
        return text;
    }
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    if (!item) return {};
    std::wstring text = pulse::path::FriendlyPathText(item->path);
    if (Reach().Offline(item->path)) {
        text += L"\n";
        text += l10n::Get(l10n::StringId::FavoritesBarUnreachable);
    }
    return text;
}
} // namespace pulse
