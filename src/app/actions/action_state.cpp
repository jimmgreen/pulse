// action_state.cpp - get_state, the selector and waiting (action_state.h).
#include "action_state.h"

#include "../app_internal.h"
#include "../app_navigation.h"
#include "../entry_group.h"
#include "../favorites_bar.h"
#include "../places.h"
#include "../window_registry.h"
#include "../../fs/fs_enum.h"
#include "../../ui/view_layout.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace pulse::app::actions {

namespace {

constexpr uint64_t kTicksPerDay = 864000000000ULL;  // 100 ns ticks

std::string U8(std::wstring_view w) { return json::Utf8(w); }

std::wstring Fold(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// The path as people write it: no \\?\ (or \\?\UNC\) prefix.
std::wstring Shown(std::wstring p) {
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
    return p;
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= p.size() ? p : p.substr(slash + 1);
}

std::wstring EntryPath(const Tab& tab, const fs::DirEntry& e) {
    if (!e.full_path.empty()) return e.full_path;
    if (tab.current_path.empty() || fs::IsVirtualPath(tab.current_path)) return {};
    std::wstring full = tab.current_path;
    if (full.back() != L'\\') full += L'\\';
    return full + e.name;
}

bool RealFolder(const Tab& tab) {
    return !tab.current_path.empty() && !fs::IsVirtualPath(tab.current_path);
}

const char* LayoutName(LayoutPreset preset) {
    switch (preset) {
    case LayoutPreset::Single: return "single";
    case LayoutPreset::TwoVertical: return "side_by_side";
    case LayoutPreset::TwoHorizontal: return "stacked";
    case LayoutPreset::Three: return "three";
    case LayoutPreset::FourGrid: return "four";
    case LayoutPreset::Custom: break;
    }
    return "custom";
}

const char* SortName(ui::SortColumn column) {
    switch (column) {
    case ui::SortColumn::Name: return "name";
    case ui::SortColumn::Mtime: return "modified";
    case ui::SortColumn::Type: return "type";
    case ui::SortColumn::Size: return "size";
    case ui::SortColumn::Path: return "path";
    case ui::SortColumn::Created: return "created";
    case ui::SortColumn::Accessed: return "accessed";
    case ui::SortColumn::MediaTaken: return "date_taken";
    case ui::SortColumn::MediaDims: return "dimensions";
    case ui::SortColumn::MediaCamera: return "camera";
    case ui::SortColumn::MediaTitle: return "title";
    case ui::SortColumn::MediaArtist: return "artist";
    case ui::SortColumn::MediaAlbum: return "album";
    case ui::SortColumn::MediaDuration: return "length";
    case ui::SortColumn::MediaFps: return "frame_rate";
    }
    return "name";
}

const char* GroupName(int group) {
    switch (GroupByFromInt(group)) {
    case GroupBy::None: return "none";
    case GroupBy::Name: return "name";
    case GroupBy::Date: return "date";
    case GroupBy::Type: return "type";
    case GroupBy::Size: return "size";
    case GroupBy::Tag: return "tag";
    case GroupBy::Location: return "location";
    case GroupBy::ThisPc: return "this_pc";
    case GroupBy::Network: return "network";
    case GroupBy::AiDup: return "duplicates";
    }
    return "none";
}

const char* OpName(ops::OpType type) {
    switch (type) {
    case ops::OpType::Copy: return "copy";
    case ops::OpType::Move: return "move";
    case ops::OpType::RecycleDelete: return "recycle";
    case ops::OpType::RealDelete: return "delete";
    case ops::OpType::Rename: return "rename";
    case ops::OpType::CreateFolder: return "new_folder";
    case ops::OpType::CreateTextFile: return "new_file";
    case ops::OpType::RestoreRecycle: return "restore";
    case ops::OpType::EmptyRecycle: return "empty_recycle_bin";
    case ops::OpType::BatchRename: return "batch_rename";
    }
    return "other";
}

const char* PhaseName(ops::OpPhase phase) {
    switch (phase) {
    case ops::OpPhase::Queued: return "queued";
    case ops::OpPhase::Scanning: return "scanning";
    case ops::OpPhase::WaitingForConflict: return "waiting_for_conflict";
    case ops::OpPhase::Running: return "running";
    case ops::OpPhase::Paused: return "paused";
    case ops::OpPhase::Verifying: return "verifying";
    case ops::OpPhase::Cancelling: return "cancelling";
    case ops::OpPhase::Completed: return "completed";
    case ops::OpPhase::Failed: return "failed";
    }
    return "running";
}

std::string ViewName(const Tab& tab) {
    std::wstring name = ui::ViewModeName(tab.view_mode);
    std::replace(name.begin(), name.end(), L'-', L'_');
    return U8(name);
}

// What a tab is called on its tab strip.
std::wstring LayoutTitle(const LayoutTab& layout) {
    if (!layout.title.empty()) return layout.title;
    const Tab* folder = layout.ActiveFolder();
    if (!folder) return {};
    if (!folder->virtual_title.empty()) return folder->virtual_title;
    if (folder->current_path.empty()) return L"This PC";
    const std::wstring title = TabTitle(folder->current_path);
    return title.empty() ? Leaf(Shown(folder->current_path)) : title;
}

// The pane Ctrl+Alt+D marked as the destination, or null.
const Pane* TargetPane(const LayoutTab& layout) {
    if (layout.target_index >= 0 && static_cast<size_t>(layout.target_index) < layout.panes.size())
        return layout.panes[static_cast<size_t>(layout.target_index)].get();
    for (const auto& p : layout.panes)
        if (p && p->target) return p.get();
    return nullptr;
}

Pane* PaneByNumber(AppState& s, int number, int* screen_number) {
    LayoutTab& layout = LiveLayout(s);
    const std::vector<Pane*> panes = PanesOnScreen(layout);
    Pane* pick = nullptr;
    if (number > 0) {
        if (static_cast<size_t>(number) <= panes.size()) pick = panes[static_cast<size_t>(number) - 1];
    } else if (number < 0) {
        for (Pane* p : panes)
            if (p == TargetPane(layout)) pick = p;
    } else {
        pick = layout.FocusedPane();
    }
    if (screen_number) {
        *screen_number = 0;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i] == pick) *screen_number = static_cast<int>(i) + 1;
    }
    return pick;
}

json::Value PaneState(AppState& s, const Pane& pane, int number, bool focused, bool target) {
    const Tab& tab = pane.view;
    json::Value out = json::Value::Object();
    out.set("pane", number);
    if (focused) out.set("focused", true);
    if (target) out.set("target", true);
    if (RealFolder(tab)) {
        out.set("folder", U8(Shown(tab.current_path)));
        const std::wstring name = TabTitle(tab.current_path);   // 下载 for Downloads
        if (!name.empty()) out.set("folder_name", U8(name));
    } else {
        out.set("folder", nullptr);
        out.set("location", U8(!tab.virtual_title.empty() ? tab.virtual_title
                               : tab.current_path.empty() ? std::wstring(L"This PC")
                                                          : tab.current_path));
    }
    out.set("view", ViewName(tab));
    if (tab.wall) out.set("photo_wall", true);
    json::Value sort = json::Value::Array();
    sort.push(SortName(tab.sort_column));
    sort.push(tab.sort_direction == ui::SortDirection::Asc ? "asc" : "desc");
    out.set("sort", std::move(sort));
    out.set("group", GroupName(tab.EffectiveGroup()));
    out.set("filter", U8(tab.filter_text));
    out.set("loading", tab.loading);

    json::Value items = json::Value::Object();
    if (tab.content_results) {
        items.set("shown", static_cast<uint64_t>(tab.EntryCount()));
        items.set("search_complete", tab.content_count_final && !tab.search_content_active);
    } else {
        std::vector<int> shown;
        CollectFilterMatches(tab, &s.places, shown);
        items.set("folders", static_cast<uint64_t>(tab.directory_count));
        items.set("files", static_cast<uint64_t>(tab.file_count));
        items.set("shown", static_cast<uint64_t>(shown.size()));
        if (IsSearchTab(tab)) {
            // Scrolling loads the rest; select.items waits for all of it.
            items.set("search_total", static_cast<uint64_t>(tab.search_total));
            items.set("search_complete", !tab.loading && !tab.search_awaiting_content &&
                                             tab.search_next_offset >= tab.search_total);
        }
    }
    out.set("items", std::move(items));

    json::Value selected = json::Value::Object();
    const int count = tab.SelectedCount();
    selected.set("count", static_cast<int64_t>(count));
    if (count > 0) {
        const size_t cap = focused ? kStateFocusedPaths : kStateOtherNames;
        json::Value list = json::Value::Array();
        for (int index : tab.SelectedIndices()) {
            if (list.items().size() >= cap) break;
            const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(index));
            if (e.name.empty()) continue;  // a search row not fetched yet
            if (focused) {
                const std::wstring path = EntryPath(tab, e);
                list.push(U8(Shown(path.empty() ? e.name : path)));
            } else {
                list.push(U8(e.name));
            }
        }
        selected.set(focused ? "paths" : "names", std::move(list));
    }
    out.set("selected", std::move(selected));
    if (tab.selected_index >= 0 && static_cast<size_t>(tab.selected_index) < tab.EntryCount()) {
        const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(tab.selected_index));
        if (!e.name.empty()) out.set("focus_item", U8(e.name));
    }
    return out;
}

// The Pulse dialog that holds the window modal (its owner is disabled), or "".
std::wstring ModalDialogTitle(const AppState& s) {
    if (!s.hwnd || IsWindowEnabled(s.hwnd)) return {};
    struct Find { HWND owner; std::wstring title; } find{s.hwnd, {}};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<Find*>(p);
        if (!IsWindowVisible(h) || !IsWindowEnabled(h) || GetWindow(h, GW_OWNER) != f->owner) return TRUE;
        wchar_t text[256]{};
        if (GetWindowTextW(h, text, ARRAYSIZE(text)) <= 0) return TRUE;
        f->title = text;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&find));
    return find.title.empty() ? std::wstring(L"dialog") : find.title;
}

// ---- Dates -------------------------------------------------------------------

FILETIME Ft(uint64_t ticks) {
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
}

// Local wall-clock ticks of a UTC instant, and back.
uint64_t ToLocal(uint64_t utc) {
    FILETIME u = Ft(utc);
    SYSTEMTIME su{}, sl{};
    if (!FileTimeToSystemTime(&u, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl)) return utc;
    FILETIME l{};
    return SystemTimeToFileTime(&sl, &l) ? FileTimeTicks(l) : utc;
}

uint64_t ToUtc(uint64_t local) {
    FILETIME l = Ft(local);
    SYSTEMTIME sl{}, su{};
    if (!FileTimeToSystemTime(&l, &sl) || !TzSpecificLocalTimeToSystemTime(nullptr, &sl, &su)) return local;
    FILETIME u{};
    return SystemTimeToFileTime(&su, &u) ? FileTimeTicks(u) : local;
}

// Local ticks of midnight on y-m-1 (month may be 0 or 13: the year rolls).
uint64_t LocalMonthStart(int year, int month) {
    while (month < 1) { month += 12; --year; }
    while (month > 12) { month -= 12; ++year; }
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = 1;
    FILETIME ft{};
    return SystemTimeToFileTime(&st, &ft) ? FileTimeTicks(ft) : 0;
}

// "2026-10-01" -> UTC ticks of that local midnight.
bool ParseDay(std::string_view text, uint64_t& out) {
    int y = 0, m = 0, d = 0;
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (size_t i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) return false;
    y = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
    m = (text[5] - '0') * 10 + (text[6] - '0');
    d = (text[8] - '0') * 10 + (text[9] - '0');
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft{};
    if (m < 1 || m > 12 || d < 1 || d > 31 || !SystemTimeToFileTime(&st, &ft)) return false;
    out = ToUtc(FileTimeTicks(ft));
    return true;
}

bool ParseModified(const json::Value& v, uint64_t now_utc, Selector& out, std::string& error) {
    out.has_modified = true;
    if (v.is_object()) {
        const std::string after = v.get_string("after"), before = v.get_string("before");
        if (after.empty() && before.empty()) { error = "modified needs after or before"; return false; }
        if (!after.empty() && !ParseDay(after, out.modified_from)) { error = "modified.after must be YYYY-MM-DD"; return false; }
        if (!before.empty()) {
            uint64_t day = 0;
            if (!ParseDay(before, day)) { error = "modified.before must be YYYY-MM-DD"; return false; }
            out.modified_to = LocalMidnightUtc(day, 1);  // the whole last day
        }
        return true;
    }
    const std::string word = v.as_string();
    const uint64_t local = ToLocal(now_utc);
    SYSTEMTIME today{};
    FILETIME lf = Ft(local);
    FileTimeToSystemTime(&lf, &today);
    if (word == "today") {
        out.modified_from = LocalMidnightUtc(now_utc, 0);
    } else if (word == "yesterday") {
        out.modified_from = LocalMidnightUtc(now_utc, -1);
        out.modified_to = LocalMidnightUtc(now_utc, 0);
    } else if (word == "this_week") {
        out.modified_from = LocalMidnightUtc(now_utc, -((today.wDayOfWeek + 6) % 7));
    } else if (word == "last_7_days") {
        out.modified_from = now_utc >= 7 * kTicksPerDay ? now_utc - 7 * kTicksPerDay : 0;
    } else if (word == "this_month") {
        out.modified_from = ToUtc(LocalMonthStart(today.wYear, today.wMonth));
    } else if (word == "last_month") {
        out.modified_from = ToUtc(LocalMonthStart(today.wYear, today.wMonth - 1));
        out.modified_to = ToUtc(LocalMonthStart(today.wYear, today.wMonth));
    } else {
        error = "modified: today, yesterday, this_week, last_7_days, this_month, last_month or {after, before}";
        return false;
    }
    return true;
}

bool ReadStrings(const json::Value* v, std::vector<std::wstring>& out, bool strip_dot) {
    if (!v) return true;
    auto add = [&](const json::Value& item) {
        if (!item.is_string()) return false;
        std::wstring s = Fold(json::Wide(item.as_string()));
        if (strip_dot) while (!s.empty() && s.front() == L'.') s.erase(0, 1);
        if (!s.empty()) out.push_back(std::move(s));
        return true;
    };
    if (v->is_string()) return add(*v);
    if (!v->is_array()) return false;
    for (const json::Value& item : v->items())
        if (!add(item)) return false;
    return true;
}

// Every condition but the tag; touches nothing but its arguments, so a
// content search's store can run it on its own thread.
bool MatchesEntry(const Selector& sel, const fs::DirEntry& e, const std::vector<std::wstring>& kind_exts) {
    const std::wstring name = Fold(e.name);
    if (!sel.names.empty()) {
        bool any = false;
        for (const std::wstring& n : sel.names) {
            const bool wild = n.find_first_of(L"*?") != std::wstring::npos;
            if (wild ? WildcardMatch(name, n) : name.find(n) != std::wstring::npos) { any = true; break; }
        }
        if (!any) return false;
    }
    if (!sel.items.empty() && std::find(sel.items.begin(), sel.items.end(), name) == sel.items.end())
        return false;
    const size_t dot = name.find_last_of(L'.');
    const std::wstring ext = e.is_dir || dot == std::wstring::npos ? std::wstring() : name.substr(dot + 1);
    if (!sel.extensions.empty() &&
        (e.is_dir || std::find(sel.extensions.begin(), sel.extensions.end(), ext) == sel.extensions.end()))
        return false;
    if (sel.kind == index::SearchKind::Folder && !e.is_dir) return false;
    if (sel.kind != index::SearchKind::Any && sel.kind != index::SearchKind::Folder &&
        (e.is_dir || std::find(kind_exts.begin(), kind_exts.end(), ext) == kind_exts.end()))
        return false;
    if ((sel.larger_than || sel.smaller_than) &&
        (e.is_dir || (sel.larger_than && e.size <= sel.larger_than) ||
         (sel.smaller_than && e.size >= sel.smaller_than)))
        return false;
    if (sel.has_modified) {
        const uint64_t t = FileTimeTicks(e.mtime);
        if (t < sel.modified_from || t >= sel.modified_to) return false;
    }
    return true;
}

bool Matches(const Selector& sel, const Tab& tab, const fs::DirEntry& e, const PlacesCatalog& places,
             int tag_index, const std::vector<std::wstring>& kind_exts) {
    if (!MatchesEntry(sel, e, kind_exts)) return false;
    return tag_index < 0 || places.PathHasTag(EntryPath(tab, e), tag_index);
}

// The extensions a type condition means (none for any / folder).
std::vector<std::wstring> KindExts(const Selector& sel) {
    std::vector<std::wstring> kind_exts;
    if (sel.kind == index::SearchKind::Any || sel.kind == index::SearchKind::Folder) return kind_exts;
    const std::wstring list = index::KindExtensions(sel.kind);
    size_t start = 0;
    while (start <= list.size()) {
        const size_t end = list.find(L';', start);
        const std::wstring ext = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!ext.empty()) kind_exts.push_back(Fold(ext));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return kind_exts;
}

// The folder a result is in (search rows carry full paths).
std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The selection a mode leads to, from what matched and what was selected.
std::vector<int> ModeResult(SelectMode mode, const std::vector<int>& matched, const std::vector<int>& current) {
    std::vector<int> result;
    switch (mode) {
    case SelectMode::Replace:
        result = matched;
        break;
    case SelectMode::Add: {
        std::set<int> all(current.begin(), current.end());
        all.insert(matched.begin(), matched.end());
        result.assign(all.begin(), all.end());
        break;
    }
    case SelectMode::Remove: {
        const std::set<int> drop(matched.begin(), matched.end());
        for (int index : current)
            if (!drop.count(index)) result.push_back(index);
        break;
    }
    }
    return result;
}

// ---- Waiting ---------------------------------------------------------------

struct WaitJob {
    HWND hwnd = nullptr;
    AppState* state = nullptr;
    WaitSpec spec;
    ULONGLONG started = 0;
    ULONGLONG deadline = 0;
    std::function<bool()> cancelled;
    std::function<void(AppState&, json::Value)> done;
};

std::map<UINT_PTR, WaitJob>& WaitJobs() {
    static std::map<UINT_PTR, WaitJob> jobs;
    return jobs;
}

void CALLBACK WaitTick(HWND, UINT, UINT_PTR id, DWORD) {
    auto& jobs = WaitJobs();
    const auto it = jobs.find(id);
    if (it == jobs.end()) { KillTimer(nullptr, id); return; }
    WaitJob& job = it->second;
    // Tests wait on windowless states; a real window that went away drops the wait.
    if ((job.hwnd && !IsWindow(job.hwnd)) || (job.cancelled && job.cancelled())) {
        KillTimer(nullptr, id);
        jobs.erase(it);
        return;
    }
    json::Value out = json::Value::Object();
    const bool ready = WaitReady(*job.state, job.spec, out);
    const ULONGLONG now = GetTickCount64();
    if (!ready && now < job.deadline) return;
    KillTimer(nullptr, id);
    WaitJob finished = std::move(job);
    jobs.erase(it);
    if (!ready) {
        out.set("ok", false);
        out.set("timed_out", true);
    }
    out.set("waited_ms", static_cast<uint64_t>(now - finished.started));
    finished.done(*finished.state, std::move(out));
}

} // namespace

std::wstring TabStripTitle(const LayoutTab& layout) { return LayoutTitle(layout); }

uint64_t FileTimeTicks(const FILETIME& ft) noexcept {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t LocalMidnightUtc(uint64_t utc, int days) {
    const uint64_t local = ToLocal(utc);
    const int64_t midnight = static_cast<int64_t>(local - local % kTicksPerDay) +
                             static_cast<int64_t>(days) * static_cast<int64_t>(kTicksPerDay);
    return ToUtc(static_cast<uint64_t>((std::max)(int64_t{0}, midnight)));
}

bool ParseByteSize(std::wstring_view text, uint64_t& out) {
    size_t i = 0;
    while (i < text.size() && std::iswspace(text[i])) ++i;
    double value = 0.0;
    bool digits = false, fraction = false;
    double scale = 0.1;
    for (; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c >= L'0' && c <= L'9') {
            digits = true;
            if (fraction) { value += (c - L'0') * scale; scale /= 10.0; }
            else value = value * 10.0 + (c - L'0');
        } else if (c == L'.' && !fraction) {
            fraction = true;
        } else {
            break;
        }
    }
    if (!digits) return false;
    while (i < text.size() && std::iswspace(text[i])) ++i;
    std::wstring unit = Fold(std::wstring(text.substr(i)));
    while (!unit.empty() && std::iswspace(unit.back())) unit.pop_back();
    double mult = 1.0;
    if (unit.empty() || unit == L"b") mult = 1.0;
    else if (unit == L"k" || unit == L"kb" || unit == L"kib") mult = 1024.0;
    else if (unit == L"m" || unit == L"mb" || unit == L"mib") mult = 1024.0 * 1024.0;
    else if (unit == L"g" || unit == L"gb" || unit == L"gib") mult = 1024.0 * 1024.0 * 1024.0;
    else if (unit == L"t" || unit == L"tb" || unit == L"tib") mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else return false;
    const double bytes = value * mult;
    if (bytes < 0.0 || bytes > 1.8e19) return false;
    out = static_cast<uint64_t>(std::llround(bytes));
    return true;
}

bool WildcardMatch(std::wstring_view name, std::wstring_view pattern) {
    // Greedy * with one backtrack point: linear for the usual patterns.
    size_t n = 0, p = 0, star = std::wstring_view::npos, mark = 0;
    auto lower = [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); };
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || lower(pattern[p]) == lower(name[n]))) {
            ++n; ++p;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            mark = n;
        } else if (star != std::wstring_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

std::vector<Pane*> PanesOnScreen(const LayoutTab& layout) {
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    if (layout.root) LayoutSplitTree(*layout.root, D2D1::RectF(0, 0, 1200, 1200), 0.0f, laid);
    std::stable_sort(laid.begin(), laid.end(), [](const auto& a, const auto& b) {
        if (std::fabs(a.second.top - b.second.top) > 1.0f) return a.second.top < b.second.top;
        return a.second.left < b.second.left;
    });
    std::vector<Pane*> out;
    out.reserve(laid.size());
    for (const auto& entry : laid) out.push_back(entry.first);
    return out;
}

bool PanesForArg(AppState& s, const json::Value* pane, std::vector<Pane*>& out, std::string& error) {
    out.clear();
    int number = 0;
    int64_t n = 0;
    if (!pane || pane->is_null() || (pane->is_string() && pane->as_string() == "focused")) {
        number = 0;
    } else if (pane->is_string() && pane->as_string() == "all") {
        out = PanesOnScreen(LiveLayout(s));
        if (out.empty()) error = "no pane";
        return !out.empty();
    } else if (pane->is_string() && pane->as_string() == "target") {
        number = -1;
    } else if (pane->get_int(n, 1, 4)) {
        number = static_cast<int>(n);
    } else {
        error = "pane: 1-4, \"focused\", \"target\" or \"all\"";
        return false;
    }
    Pane* p = PaneByNumber(s, number, nullptr);
    if (!p) {
        error = number < 0 ? "no target pane is marked" : "there is no such pane on screen";
        return false;
    }
    out.push_back(p);
    return true;
}

json::Value StateSnapshot(AppState& s) {
    json::Value out = json::Value::Object();
    LayoutTab& layout = LiveLayout(s);

    json::Value window = json::Value::Object();
    window.set("title", U8(LayoutTitle(layout)));
    window.set("tabs", static_cast<uint64_t>(s.window_tabs.items.size()));
    json::Value others = json::Value::Array();
    ForEachWindowSharingCore(s, [&](AppState& w) {
        if (&w == &s) return;
        const LayoutTab* lt = w.window_tabs.Active();
        json::Value other = json::Value::Object();
        other.set("title", U8(lt ? LayoutTitle(*lt) : std::wstring()));
        other.set("tabs", static_cast<uint64_t>(w.window_tabs.items.size()));
        others.push(std::move(other));
    });
    if (!others.items().empty()) window.set("other_windows", std::move(others));
    out.set("window", std::move(window));

    json::Value tab = json::Value::Object();
    tab.set("index", static_cast<uint64_t>(s.window_tabs.active + 1));
    tab.set("layout", LayoutName(layout.layout));
    if (layout.pinned) tab.set("pinned", true);
    tab.set("compare", layout.compare);
    out.set("tab", std::move(tab));

    json::Value panes = json::Value::Array();
    const std::vector<Pane*> on_screen = PanesOnScreen(layout);
    const Pane* focused = layout.FocusedPane();
    const Pane* target = TargetPane(layout);
    for (size_t i = 0; i < on_screen.size(); ++i)
        panes.push(PaneState(s, *on_screen[i], static_cast<int>(i) + 1, on_screen[i] == focused,
                             on_screen[i] == target));
    out.set("panes", std::move(panes));

    json::Value other_tabs = json::Value::Array();
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        if (i == s.window_tabs.active || !s.window_tabs.items[i]) continue;
        const LayoutTab& lt = *s.window_tabs.items[i];
        json::Value t = json::Value::Object();
        t.set("index", static_cast<uint64_t>(i + 1));
        t.set("title", U8(LayoutTitle(lt)));
        t.set("layout", LayoutName(lt.layout));
        other_tabs.push(std::move(t));
    }
    if (!other_tabs.items().empty()) out.set("other_tabs", std::move(other_tabs));

    json::Value tray = json::Value::Object();
    json::Value tray_names = json::Value::Array();
    uint64_t tray_count = 0;
    for (const TrayBatch& batch : s.tray.batches())
        for (const TrayItem& item : batch.items) {
            ++tray_count;
            if (tray_names.items().size() < kStateTrayNames) tray_names.push(U8(Leaf(item.path)));
        }
    tray.set("count", tray_count);
    if (tray_count) tray.set("names", std::move(tray_names));
    out.set("tray", std::move(tray));

    if (s.quickPreview.visible()) {
        json::Value preview = json::Value::Object();
        preview.set("path", U8(Shown(s.quickPreview.item().path)));
        out.set("preview", std::move(preview));
    } else {
        out.set("preview", nullptr);
    }

    const std::wstring dialog = ModalDialogTitle(s);
    if (dialog.empty()) out.set("dialog", nullptr);
    else {
        json::Value d = json::Value::Object();
        d.set("title", U8(dialog));
        out.set("dialog", std::move(d));
    }

    json::Value panels = json::Value::Object();
    panels.set("sidebar", !s.appPrefs.sidebar_collapsed);
    panels.set("details", s.showDetailsPanel);
    panels.set("favorites_bar", FavoritesBarShown(s));
    out.set("panels", std::move(panels));

    json::Value file_ops = json::Value::Object();
    const ops::OpStatus status = s.ops.Status();
    file_ops.set("active", status.active);
    if (status.active) {
        file_ops.set("task", status.task_id);
        file_ops.set("type", OpName(status.type));
        file_ops.set("phase", PhaseName(status.phase));
        if (status.percent >= 0.0f) file_ops.set("percent", static_cast<int64_t>(std::lround(status.percent)));
    }
    out.set("file_ops", std::move(file_ops));
    return out;
}

bool ParseSelector(const json::Value& args, uint64_t now_utc, Selector& out, std::string& error) {
    out = Selector{};
    if (!args.is_object()) { error = "the selector must be an object"; return false; }
    if (const json::Value* pane = args.find("pane")) {
        int64_t n = 0;
        if (pane->is_string() && pane->as_string() == "focused") out.pane = 0;
        else if (pane->is_string() && pane->as_string() == "target") out.pane = -1;
        else if (pane->get_int(n, 1, 4)) out.pane = static_cast<int>(n);
        else { error = "pane: 1-4, \"focused\" or \"target\""; return false; }
    }
    if (!ReadStrings(args.find("name"), out.names, false)) { error = "name: a string or a list of strings"; return false; }
    if (!ReadStrings(args.find("extensions"), out.extensions, true)) { error = "extensions: a list of strings"; return false; }
    if (!ReadStrings(args.find("items"), out.items, false)) { error = "items: a list of names"; return false; }
    if (const json::Value* type = args.find("type")) {
        out.kind = type->is_string() ? index::ParseKindId(json::Wide(type->as_string())) : index::SearchKind::Any;
        if (out.kind == index::SearchKind::Any || out.kind == index::SearchKind::Custom) {
            error = "type: folder, document, image, video, audio, archive or code";
            return false;
        }
    }
    if (const json::Value* modified = args.find("modified"))
        if (!ParseModified(*modified, now_utc, out, error)) return false;
    if (const json::Value* size = args.find("size")) {
        if (!size->is_object()) { error = "size: {\"larger_than\":\"100MB\"} or smaller_than"; return false; }
        const std::string larger = size->get_string("larger_than"), smaller = size->get_string("smaller_than");
        if ((!larger.empty() && !ParseByteSize(json::Wide(larger), out.larger_than)) ||
            (!smaller.empty() && !ParseByteSize(json::Wide(smaller), out.smaller_than)) ||
            (larger.empty() && smaller.empty())) {
            error = "size: sizes such as 500KB, 100MB or 1.5GB";
            return false;
        }
    }
    if (const json::Value* tag = args.find("tag")) {
        if (!tag->is_string() || tag->as_string().empty()) { error = "tag: a tag's name"; return false; }
        out.tag = json::Wide(tag->as_string());
    }
    out.within_selection = args.get_bool("selected");
    const std::string mode = args.get_string("mode", "replace");
    if (mode == "replace") out.mode = SelectMode::Replace;
    else if (mode == "add") out.mode = SelectMode::Add;
    else if (mode == "remove") out.mode = SelectMode::Remove;
    else { error = "mode: replace, add or remove"; return false; }
    const bool any = !out.names.empty() || !out.extensions.empty() || !out.items.empty() ||
                     out.kind != index::SearchKind::Any || out.has_modified || out.larger_than ||
                     out.smaller_than || !out.tag.empty() || out.within_selection;
    if (!any) { error = "give at least one condition"; return false; }
    return true;
}

bool IsSearchTab(const Tab& tab) {
    if (tab.content_results) return true;
    std::wstring kind;
    return ParsePulsePath(tab.current_path, &kind, nullptr) && (kind == L"search" || kind == L"saved-search");
}

bool PaneShowsSearch(AppState& s, int pane) {
    const Pane* p = PaneByNumber(s, pane, nullptr);
    return p && IsSearchTab(p->view);
}

// The tag a selector names: its index, -1 for none, -2 when unknown.
static int TagIndex(const AppState& s, const Selector& sel) {
    if (sel.tag.empty()) return -1;
    const std::wstring want = Fold(sel.tag);
    for (size_t i = 0; i < s.places.tags.size(); ++i)
        if (Fold(s.places.tags[i].name) == want) return static_cast<int>(i);
    return -2;
}

SelectorMatch ResolveSelector(AppState& s, const Selector& sel) {
    SelectorMatch m;
    m.pane = PaneByNumber(s, sel.pane, &m.pane_number);
    if (!m.pane) { m.reason = "no_pane"; return m; }
    Tab& tab = m.pane->view;
    m.results = IsSearchTab(tab);
    if (tab.loading) { m.reason = "loading"; return m; }  // an old listing may still be up
    // Its rows come page by page from a store: ResolveSelectorAsync.
    if (tab.content_results) { m.reason = "search_results"; return m; }
    const int tag_index = TagIndex(s, sel);
    if (tag_index == -2) { m.reason = "unknown_tag"; return m; }
    const std::vector<std::wstring> kind_exts = KindExts(sel);
    std::vector<int> shown;
    CollectFilterMatches(tab, &s.places, shown);
    const std::set<int> visible(shown.begin(), shown.end());
    const std::vector<int> current = tab.SelectedIndices();
    const std::set<int> chosen(current.begin(), current.end());
    std::set<std::wstring> folders;
    const size_t n = tab.EntryCount();
    for (size_t i = 0; i < n; ++i) {
        const int index = static_cast<int>(i);
        if (sel.within_selection && !chosen.count(index)) continue;
        const fs::DirEntry e = tab.EntryAt(i);
        if (e.name.empty() || !tab.AllowsAttributes(e.attrs)) continue;
        if (!Matches(sel, tab, e, s.places, tag_index, kind_exts)) continue;
        if (!visible.count(index)) { ++m.hidden_by_filter; continue; }
        m.matched.push_back(index);
        if (!e.is_dir) m.bytes += e.size;
        if (m.results) folders.insert(Fold(ParentOf(e.full_path)));
    }
    m.folders = m.results ? folders.size() : (m.matched.empty() ? 0 : 1);
    if (m.matched.empty()) {
        m.reason = m.hidden_by_filter ? "filter_hides" : "no_match";
        return m;
    }
    m.result = ModeResult(sel.mode, m.matched, current);
    return m;
}

namespace {

// A content search's match: the store's thread fills it, the UI thread reads
// it once `done` is set (the mutex orders the two).
struct MatchSlot {
    std::mutex mutex;
    bool done = false;
    std::string reason;                    // set before the match: why not
    index::ContentResultStore::Selection result;
    std::vector<uint64_t> sizes;           // per match, in order
    std::vector<std::wstring> parents;
};

struct MatchJob {
    HWND hwnd = nullptr;
    AppState* state = nullptr;
    Pane* pane = nullptr;
    int pane_number = 0;
    std::weak_ptr<index::ContentResultStore> store;
    uint64_t order = 0;
    Selector sel;
    std::shared_ptr<MatchSlot> slot;
    std::function<void(AppState&, SelectorMatch)> done;
};

std::map<UINT_PTR, MatchJob>& MatchJobs() {
    static std::map<UINT_PTR, MatchJob> jobs;
    return jobs;
}

void CALLBACK MatchTick(HWND, UINT, UINT_PTR id, DWORD) {
    auto& jobs = MatchJobs();
    const auto it = jobs.find(id);
    if (it == jobs.end()) { KillTimer(nullptr, id); return; }
    if (it->second.hwnd && !IsWindow(it->second.hwnd)) {
        KillTimer(nullptr, id);
        jobs.erase(it);
        return;
    }
    {
        std::lock_guard lock(it->second.slot->mutex);
        if (!it->second.slot->done) return;
    }
    KillTimer(nullptr, id);
    MatchJob job = std::move(it->second);
    jobs.erase(it);
    AppState& s = *job.state;
    MatchSlot& slot = *job.slot;
    SelectorMatch m;
    m.results = true;
    m.pane_number = job.pane_number;
    int number = 0;
    Pane* pane = PaneByNumber(s, job.pane_number, &number);
    const auto store = job.store.lock();
    // The pane, its search or the order of its rows changed meanwhile.
    if (pane != job.pane || !store || pane->view.content_results != store || store->OrderRevision() != job.order) {
        m.reason = "changed";
        job.done(s, std::move(m));
        return;
    }
    m.pane = pane;
    if (!slot.reason.empty() || slot.result.error) {
        m.reason = !slot.reason.empty() ? slot.reason : "search_failed";
        job.done(s, std::move(m));
        return;
    }
    Tab& tab = pane->view;
    const std::vector<int> current = tab.SelectedIndices();
    const std::set<int> chosen(current.begin(), current.end());
    std::set<std::wstring> folders;
    const std::vector<int>& found = slot.result.matches;
    for (size_t i = 0; i < found.size(); ++i) {
        if (job.sel.within_selection && !chosen.count(found[i])) continue;
        m.matched.push_back(found[i]);
        if (i < slot.sizes.size()) m.bytes += slot.sizes[i];
        if (i < slot.parents.size()) folders.insert(Fold(slot.parents[i]));
    }
    m.folders = folders.size();
    if (m.matched.empty()) m.reason = "no_match";   // the filter box already shaped the rows
    else m.result = ModeResult(job.sel.mode, m.matched, current);
    job.done(s, std::move(m));
}

} // namespace

bool ResolveSelectorAsync(AppState& s, const Selector& sel, std::function<void(AppState&, SelectorMatch)> done) {
    int number = 0;
    Pane* pane = PaneByNumber(s, sel.pane, &number);
    if (!pane || !pane->view.content_results) return false;
    Tab& tab = pane->view;
    auto slot = std::make_shared<MatchSlot>();
    const auto store = tab.content_results;
    if (!sel.tag.empty()) slot->reason = "tag_in_results";
    else if (tab.loading) slot->reason = "loading";
    slot->done = !slot->reason.empty();
    MatchJob job;
    job.hwnd = s.hwnd;
    job.state = &s;
    job.pane = pane;
    job.pane_number = number;
    job.store = store;
    job.order = store->OrderRevision();
    job.sel = sel;
    job.slot = slot;
    job.done = std::move(done);
    // Always from the timer, never from here: callers go on after this returns.
    const UINT_PTR id = SetTimer(nullptr, 0, 30, MatchTick);
    if (!id) return false;
    MatchJobs()[id] = std::move(job);
    if (slot->done) return true;
    // Its own copies only: the filter runs on the store's thread.
    const bool hidden = tab.show_hidden_files;
    const bool system = tab.show_protected_os_files;
    store->Match(
        [sel, kinds = KindExts(sel), slot, hidden, system](const fs::DirEntry& e) {
            if (e.name.empty()) return false;
            if ((e.attrs & FILE_ATTRIBUTE_HIDDEN) && (!hidden || ((e.attrs & FILE_ATTRIBUTE_SYSTEM) && !system)))
                return false;   // as Tab::AllowsAttributes
            if (!MatchesEntry(sel, e, kinds)) return false;
            slot->sizes.push_back(e.is_dir ? 0 : e.size);
            slot->parents.push_back(ParentOf(e.full_path));
            return true;
        },
        [slot](index::ContentResultStore::Selection result) {
            std::lock_guard lock(slot->mutex);
            slot->result = std::move(result);
            slot->done = true;
        });
    return true;
}

json::Value ApplySelection(AppState& s, const SelectorMatch& m) {
    json::Value out = json::Value::Object();
    out.set("pane", m.pane_number);
    out.set("matched", static_cast<uint64_t>(m.matched.size()));
    if (!m.pane) {
        out.set("reason", m.reason);
        return out;
    }
    Tab& tab = m.pane->view;
    if (!m.matched.empty()) {
        if (m.result.empty()) tab.ClearSelection();
        else tab.SelectIndices(m.result);
        if (s.hwnd) {
            const int first = m.matched.front();
            if (std::find(m.result.begin(), m.result.end(), first) != m.result.end())
                EnsureRowVisible(s, tab, first);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        json::Value names = json::Value::Array();
        for (int index : m.matched) {
            if (names.items().size() >= kSelectionNames) break;
            const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(index));
            if (!e.name.empty()) names.push(U8(e.name));   // a search row not fetched yet
        }
        out.set("names", std::move(names));
        out.set("bytes", m.bytes);
        if (m.results) out.set("folders", static_cast<uint64_t>(m.folders));
    } else {
        out.set("reason", m.reason);
        if (m.hidden_by_filter) out.set("hidden_by_filter", static_cast<uint64_t>(m.hidden_by_filter));
    }
    out.set("selected", static_cast<int64_t>(tab.SelectedCount()));
    return out;
}

bool ParseWait(const json::Value& args, WaitSpec& out, std::string& error) {
    out = WaitSpec{};
    const std::string kind = args.get_string("wait");
    uint32_t max_ms = 0;
    if (kind == "listing_loaded") { out.kind = WaitKind::ListingLoaded; out.timeout_ms = 5000; max_ms = 30000; }
    else if (kind == "op_finished") { out.kind = WaitKind::OpFinished; out.timeout_ms = 10000; max_ms = 60000; }
    else if (kind == "preview_open") { out.kind = WaitKind::PreviewOpen; out.timeout_ms = 3000; max_ms = 10000; }
    else if (kind == "search_done") { out.kind = WaitKind::SearchDone; out.timeout_ms = 10000; max_ms = 30000; }
    else { error = "wait: listing_loaded, op_finished, preview_open or search_done"; return false; }
    if (const json::Value* pane = args.find("pane")) {
        int64_t n = 0;
        if (!pane->get_int(n, 1, 4)) { error = "pane: 1-4"; return false; }
        out.pane = static_cast<int>(n);
    }
    if (const json::Value* task = args.find("task")) {
        int64_t n = 0;
        if (!task->get_int(n, 1)) { error = "task: a task number from file_ops"; return false; }
        out.task = static_cast<uint64_t>(n);
    }
    if (const json::Value* timeout = args.find("timeout_ms")) {
        int64_t n = 0;
        if (!timeout->get_int(n, 0)) { error = "timeout_ms: milliseconds"; return false; }
        out.timeout_ms = static_cast<uint32_t>((std::min)(static_cast<int64_t>(max_ms), n));
    }
    return true;
}

bool WaitReady(AppState& s, const WaitSpec& spec, json::Value& out) {
    if (!out.is_object()) out = json::Value::Object();
    switch (spec.kind) {
    case WaitKind::ListingLoaded: {
        int number = 0;
        Pane* pane = PaneByNumber(s, spec.pane, &number);
        if (!pane) {
            out.set("ok", false);
            out.set("reason", "no_pane");
            return true;
        }
        const Tab& tab = pane->view;
        out.set("pane", number);
        out.set("loaded", static_cast<uint64_t>(tab.EntryCount()));
        if (tab.loading || (!tab.snapshot && !tab.content_results)) return false;
        out.set("ok", true);
        return true;
    }
    case WaitKind::SearchDone: {
        int number = 0;
        Pane* pane = PaneByNumber(s, spec.pane, &number);
        if (!pane) {
            out.set("ok", false);
            out.set("reason", "no_pane");
            return true;
        }
        Tab& tab = pane->view;
        out.set("pane", number);
        out.set("ok", true);
        if (!IsSearchTab(tab)) return true;   // a folder: nothing to wait for
        out.set("results", static_cast<uint64_t>(tab.EntryCount()));
        if (tab.content_results) {
            out.set("total", static_cast<uint64_t>(tab.content_results->Count()));
            if (tab.loading || tab.search_awaiting_content || tab.search_content_active ||
                tab.content_results->Filtering() || tab.content_results->Sorting() ||
                tab.content_order_revision != tab.content_results->OrderRevision())
                return false;
            out.set("complete", tab.content_count_final);
            return true;
        }
        out.set("total", static_cast<uint64_t>(tab.search_total));
        if (tab.loading || tab.search_awaiting_content || tab.search_content_active) return false;
        // Loads the pages scrolling has not (a side effect of asking).
        if (RequestAllSearchResults(s, tab)) return false;
        out.set("complete", !tab.search_content_stopped && tab.search_next_offset >= tab.search_total);
        return true;
    }
    case WaitKind::OpFinished: {
        if (s.ops.PendingConflict()) {
            // The user answers conflicts; the assistant never picks for them.
            out.set("ok", false);
            out.set("dialog", "conflict");
            return true;
        }
        const ops::OpStatus now = s.ops.Status();
        if (spec.task == 0) {
            if (now.active) {
                out.set("task", now.task_id);
                out.set("done_bytes", now.transferred_bytes);
                out.set("total_bytes", now.total_bytes);
                return false;
            }
            out.set("ok", true);
            return true;
        }
        out.set("task", spec.task);
        if (const auto final_status = s.ops.FinalStatus(spec.task)) {
            out.set("ok", final_status->phase == ops::OpPhase::Completed && final_status->last_error.empty());
            out.set("phase", PhaseName(final_status->phase));
            out.set("items", final_status->completed_items);
            if (!final_status->failures.empty())
                out.set("failed", static_cast<uint64_t>(final_status->failures.size()));
            if (!final_status->last_error.empty()) out.set("error", U8(final_status->last_error));
            return true;
        }
        // Task numbers only grow: a newer one than the current task is queued.
        if (now.task_id == spec.task || spec.task > now.task_id) {
            out.set("phase", now.task_id == spec.task ? PhaseName(now.phase) : "queued");
            out.set("done_bytes", now.transferred_bytes);
            out.set("total_bytes", now.total_bytes);
            return false;
        }
        out.set("ok", false);
        out.set("reason", "unknown_task");
        return true;
    }
    case WaitKind::PreviewOpen:
        if (!s.quickPreview.visible()) return false;
        out.set("ok", true);
        out.set("path", U8(Shown(s.quickPreview.item().path)));
        return true;
    }
    return true;
}

void WaitFor(AppState& s, const WaitSpec& spec, std::function<bool()> cancelled,
             std::function<void(AppState&, json::Value)> done) {
    json::Value out = json::Value::Object();
    if (WaitReady(s, spec, out)) {
        out.set("waited_ms", 0);
        done(s, std::move(out));
        return;
    }
    const UINT_PTR id = spec.timeout_ms ? SetTimer(nullptr, 0, 50, WaitTick) : 0;
    if (!id) {
        out.set("ok", false);
        out.set("timed_out", true);
        out.set("waited_ms", 0);
        done(s, std::move(out));
        return;
    }
    WaitJob job;
    job.hwnd = s.hwnd;
    job.state = &s;
    job.spec = spec;
    job.started = GetTickCount64();
    job.deadline = job.started + spec.timeout_ms;
    job.cancelled = std::move(cancelled);
    job.done = std::move(done);
    WaitJobs()[id] = std::move(job);
}

} // namespace pulse::app::actions
