// action_args.cpp - The runners of action_args.h, by domain.
#include "action_args.h"
#include "../window_arrange_ui.h"
#include "../window_management_ui.h"

#include "action_inventory.h"
#include "action_state.h"
#include "../app_commands.h"
#include "../app_internal.h"
#include "../app_navigation.h"
#include "../app_ops_ui.h"
#include "../app_runtime.h"
#include "../folder_alias.h"
#include "../keymap.h"
#include "../places.h"
#include "../reopen_closed.h"
#include "../saved_search.h"
#include "../shortcut_page.h"
#include "../tab_controller.h"
#include "../tag_query.h"
#include "../window_registry.h"
#include "../../common/localization.h"
#include "../../fs/fs_enum.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <memory>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }
std::wstring W(const std::string& s) { return json::Wide(s); }
std::string U8(const std::wstring& s) { return json::Utf8(s); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

std::wstring Num(size_t n) { return std::to_wstring(n); }

std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}

bool SameText(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

ArgRun Fail(std::string error, I why) {
    ArgRun r;
    r.error = std::move(error);
    r.why = Text(why);
    return r;
}

// The line: `text` with `bold` drawn bold (where it first appears).
void SetLine(ArgRun& r, const std::wstring& text, const std::wstring& bold = {}) {
    const size_t at = bold.empty() ? std::wstring::npos : text.find(bold);
    if (at == std::wstring::npos) {
        r.line_before = text;
        return;
    }
    r.line_before = text.substr(0, at);
    r.line_bold = bold;
    r.line_after = text.substr(at + bold.size());
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// "a · b · c" and "+N" past three.
std::wstring NameList(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (size_t i = 0; i < names.size() && i < 3; ++i) out += (i ? L" \u00B7 " : L"") + names[i];
    if (names.size() > 3) out += L" \u00B7 +" + Num(names.size() - 3);
    return out;
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    return leaf.empty() ? path : leaf;
}

// ---- Colors ------------------------------------------------------------------

constexpr std::string_view kColorWords[] = {"red", "orange", "yellow", "green", "teal", "blue", "purple", "pink"};
constexpr I kColorNames[] = {I::ActColorRed,  I::ActColorOrange, I::ActColorYellow, I::ActColorGreen,
                             I::ActColorTeal, I::ActColorBlue,   I::ActColorPurple, I::ActColorPink};

int ColorOfRgb(uint32_t rgb) {
    const uint32_t* palette = TabController::Palette();
    for (size_t i = 0; i < TabController::PaletteSize(); ++i)
        if (palette[i] == rgb) return static_cast<int>(i);
    return -1;
}

// The color the user's tag palette has closest to `rgb`.
uint32_t NearestTagColor(AppState& s, uint32_t rgb) {
    const std::vector<uint32_t>& palette = TagColorPalette(s);
    uint32_t best = rgb;
    long best_d = -1;
    for (const uint32_t c : palette) {
        const long dr = static_cast<long>((c >> 16) & 0xFF) - static_cast<long>((rgb >> 16) & 0xFF);
        const long dg = static_cast<long>((c >> 8) & 0xFF) - static_cast<long>((rgb >> 8) & 0xFF);
        const long db = static_cast<long>(c & 0xFF) - static_cast<long>(rgb & 0xFF);
        const long d = dr * dr + dg * dg + db * db;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

// The tag color's name: the palette's own, or the nearest palette word.
std::wstring TagColorName(uint32_t rgb) {
    int idx = ColorOfRgb(rgb);
    if (idx < 0) {
        long best_d = -1;
        const uint32_t* palette = TabController::Palette();
        for (size_t i = 0; i < TabController::PaletteSize(); ++i) {
            const uint32_t c = palette[i];
            const long dr = static_cast<long>((c >> 16) & 0xFF) - static_cast<long>((rgb >> 16) & 0xFF);
            const long dg = static_cast<long>((c >> 8) & 0xFF) - static_cast<long>((rgb >> 8) & 0xFF);
            const long db = static_cast<long>(c & 0xFF) - static_cast<long>(rgb & 0xFF);
            const long d = dr * dr + dg * dg + db * db;
            if (best_d < 0 || d < best_d) {
                best_d = d;
                idx = static_cast<int>(i);
            }
        }
    }
    return ColorName(idx);
}

// ---- Tabs ----------------------------------------------------------------------

std::wstring TabName(AppState& s, size_t i) {
    return i < s.window_tabs.items.size() && s.window_tabs.items[i] ? TabStripTitle(*s.window_tabs.items[i])
                                                                    : std::wstring();
}

bool OneTabRef(AppState& s, const json::Value& v, std::vector<size_t>& out) {
    const size_t n = s.window_tabs.items.size();
    int64_t k = 0;
    if (v.get_int(k, 1, static_cast<int64_t>(n))) {
        out.push_back(static_cast<size_t>(k - 1));
        return true;
    }
    if (!v.is_string()) return false;
    const std::wstring want = Trim(W(v.as_string()));
    if (want == L"all") {
        for (size_t i = 0; i < n; ++i) out.push_back(i);
        return true;
    }
    if (want.empty() || want == L"current" || want == L"active") {
        out.push_back(s.window_tabs.active);
        return true;
    }
    for (size_t i = 0; i < n; ++i)
        if (SameText(TabName(s, i), want)) {
            out.push_back(i);
            return true;
        }
    for (size_t i = 0; i < n; ++i)
        if (TabName(s, i).find(want) != std::wstring::npos) {
            out.push_back(i);
            return true;
        }
    return false;
}

// "tab": absent (the current tab), a number (1 = first, as get_state counts),
// a title, "all", or a list of these.
bool TabsForArg(AppState& s, const json::Value& args, std::vector<size_t>& out) {
    const json::Value* v = args.find("tab");
    if (!v) v = args.find("tabs");
    if (!v || v->is_null()) {
        if (s.window_tabs.Active()) out.push_back(s.window_tabs.active);
        return !out.empty();
    }
    if (v->is_array()) {
        for (size_t i = 0; i < v->size(); ++i)
            if (!OneTabRef(s, v->at(i), out)) return false;
    } else if (!OneTabRef(s, *v, out)) {
        return false;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return !out.empty();
}

LayoutTab* TabPtr(AppState& s, size_t i) {
    return i < s.window_tabs.items.size() ? s.window_tabs.items[i].get() : nullptr;
}

// Where `tab` is now, or -1 when it was closed.
int IndexOfTab(AppState& s, const LayoutTab* tab) {
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
        if (s.window_tabs.items[i].get() == tab) return static_cast<int>(i);
    return -1;
}

std::wstring TabsLabel(AppState& s, const std::vector<size_t>& idx) {
    if (idx.size() == 1) return TabName(s, idx[0]);
    return Fill(Text(I::AiActNTabs), L"{n}", Num(idx.size()));
}

std::vector<std::wstring> TabNames(AppState& s, const std::vector<size_t>& idx) {
    std::vector<std::wstring> names;
    for (const size_t i : idx) names.push_back(TabName(s, i));
    return names;
}

// The color a pin gave a tab that had none: a later tab.color says 无 → 蓝色
// (what the user had), not the pin's color. Kept by tab and color, so a
// tab recolored or closed since no longer matches.
struct AutoMarker {
    const LayoutTab* tab;
    uint32_t rgb;
};
std::vector<AutoMarker>& AutoMarkers() {
    static std::vector<AutoMarker> marks;
    return marks;
}
void ForgetAutoMarker(const LayoutTab* tab) {
    std::erase_if(AutoMarkers(), [tab](const AutoMarker& m) { return m.tab == tab; });
}
void NoteAutoMarker(const LayoutTab* tab) {
    ForgetAutoMarker(tab);
    AutoMarkers().push_back({tab, tab->marker_rgb});
}
bool IsAutoMarker(const LayoutTab* tab, uint32_t rgb) {
    for (const AutoMarker& m : AutoMarkers())
        if (m.tab == tab && m.rgb == rgb) return true;
    return false;
}

ArgRun TabPin(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx)) return Fail("no such tab (get_state lists them)", I::AiActWhyNoTab);
    const bool pin = args.get_bool("pin", true);
    ArgRun r;
    r.arg = TabsLabel(s, idx);
    r.sub = NameList(TabNames(s, idx));
    // Pinning gives a tab without a color the next palette color and moves
    // it to the pinned end; 撤销 takes both back.
    struct Was {
        LayoutTab* tab;
        int index;
        uint32_t rgb;
    };
    std::vector<Was> changed;
    for (const size_t i : idx)
        if (LayoutTab* t = TabPtr(s, i); t && t->pinned != pin) changed.push_back({t, static_cast<int>(i), t->marker_rgb});
    r.ok = true;
    if (changed.empty()) {
        r.skip = true;
        return r;
    }
    for (const Was& w : changed) {
        const int at = IndexOfTab(s, w.tab);
        if (at >= 0) s.tabs.RunTabCommand(s.window_tabs, at, CmdTabPin);
        if (pin && w.rgb == 0 && w.tab->marker_rgb != 0) NoteAutoMarker(w.tab);
    }
    const std::wstring what = changed.size() == 1 ? TabName(s, static_cast<size_t>((std::max)(0, IndexOfTab(s, changed[0].tab))))
                                                  : Fill(Text(I::AiActNTabs), L"{n}", Num(changed.size()));
    SetLine(r, Fill(Text(pin ? I::AiActLinePinned : I::AiActLineUnpinned), L"{tabs}", what), what);
    r.data = json::Value::Object();
    r.data.set("tabs", static_cast<uint64_t>(changed.size()));
    r.data.set("pin", pin);
    r.undo = [changed, pin](AppState& st) {
        for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
            const int at = IndexOfTab(st, it->tab);
            if (at < 0 || it->tab->pinned != pin) continue;
            const uint32_t auto_rgb = it->tab->marker_rgb;
            st.tabs.RunTabCommand(st.window_tabs, at, CmdTabPin);
            if (pin && it->rgb == 0 && IsAutoMarker(it->tab, auto_rgb)) it->tab->marker_rgb = 0;
            ForgetAutoMarker(it->tab);
            const int now = IndexOfTab(st, it->tab);
            const int back = (std::min)(it->index, static_cast<int>(st.window_tabs.items.size()) - 1);
            if (now >= 0 && back >= 0 && now != back) st.window_tabs.MoveTab(static_cast<size_t>(now), static_cast<size_t>(back));
        }
        Redraw(st);
    };
    return r;
}

ArgRun TabColor(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx)) return Fail("no such tab (get_state lists them)", I::AiActWhyNoTab);
    const int color = args.find("color") ? ColorIndex(args.get_string("color")) : -2;
    if (color == -2) return Fail("color: red|orange|yellow|green|teal|blue|purple|pink|none", I::AiActWhyFailed);
    const uint32_t rgb = color >= 0 ? TabController::Palette()[color] : 0;
    ArgRun r;
    r.ok = true;
    r.arg = ColorName(color);
    r.sub = TabsLabel(s, idx);
    std::vector<std::pair<LayoutTab*, uint32_t>> before;
    for (const size_t i : idx)
        if (LayoutTab* t = TabPtr(s, i); t && t->marker_rgb != rgb) before.push_back({t, t->marker_rgb});
    if (before.empty()) {
        r.skip = true;
        return r;
    }
    // What the user had: a pin's own color counts as none.
    const uint32_t had = IsAutoMarker(before.front().first, before.front().second) ? 0 : before.front().second;
    const std::wstring from = had == 0 ? Text(I::ActColorNone) : ColorName(ColorOfRgb(had));
    for (const auto& [t, _] : before) {
        const int at = IndexOfTab(s, t);
        if (at >= 0) s.tabs.RunTabCommand(s.window_tabs, at, color >= 0 ? CmdTabColorBase + color : CmdTabColorNone);
    }
    // 标签页颜色 无 → 蓝色 (the step's sub says how many).
    SetLine(r, Text(I::TabColor) + L" " + from + L" \u2192 " + r.arg, r.arg);
    r.data = json::Value::Object();
    r.data.set("from", U8(from));
    r.data.set("to", U8(r.arg));
    r.undo = [before, rgb](AppState& st) {
        for (const auto& [t, old_rgb] : before)
            if (IndexOfTab(st, t) >= 0 && t->marker_rgb == rgb) t->marker_rgb = old_rgb;
        Redraw(st);
    };
    return r;
}

ArgRun TabRename(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx) || idx.size() != 1) return Fail("name one tab (get_state lists them)", I::AiActWhyNoTab);
    LayoutTab* t = TabPtr(s, idx[0]);
    const std::wstring name = Trim(W(args.get_string("name")));
    const std::wstring from = TabName(s, idx[0]), old_title = t->title;
    ArgRun r;
    r.ok = true;
    r.arg = name.empty() ? from : name;
    if (t->title == name) {
        r.skip = true;
        return r;
    }
    s.tabs.RunTabCommand(s.window_tabs, static_cast<int>(idx[0]), name.empty() ? CmdTabNameReset : CmdTabNameSave, name);
    const std::wstring to = TabName(s, idx[0]);
    SetLine(r, Fill(Fill(Text(I::AiActLineRenamed), L"{from}", from), L"{to}", to), to);
    r.undo = [t, name, old_title](AppState& st) {
        if (IndexOfTab(st, t) >= 0 && t->title == name) t->title = old_title;
        Redraw(st);
    };
    return r;
}

// "group": a group's name (made when there is none), or "" to leave it.
ArgRun TabGroupRun(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx)) return Fail("no such tab (get_state lists them)", I::AiActWhyNoTab);
    const std::wstring group = Trim(W(args.get_string("group")));
    struct Was {
        LayoutTab* tab;
        int group;
    };
    std::vector<Was> was;
    for (const size_t i : idx)
        if (LayoutTab* t = TabPtr(s, i); t && !t->pinned) was.push_back({t, t->tab_group});
    if (was.empty()) return Fail("pinned tabs are not grouped; unpin first", I::AiActWhyFailed);
    ArgRun r;
    r.ok = true;
    r.arg = group;
    r.sub = TabsLabel(s, idx);
    int join = -1;
    for (size_t g = 0; g < s.window_tabs.tab_groups.size(); ++g)
        if (!group.empty() && SameText(s.window_tabs.tab_groups[g].name, group)) join = static_cast<int>(g);
    for (const Was& w : was) {
        const int at = IndexOfTab(s, w.tab);
        if (at < 0) continue;
        if (group.empty()) {
            if (w.tab->tab_group != 0) s.tabs.RunTabCommand(s.window_tabs, at, CmdTabRemoveFromGroup);
        } else if (join >= 0) {
            if (w.tab->tab_group != s.window_tabs.tab_groups[static_cast<size_t>(join)].id)
                s.tabs.RunTabCommand(s.window_tabs, at, CmdTabJoinGroupBase + join);
        } else {
            s.tabs.RunTabCommand(s.window_tabs, at, CmdTabAddToNewGroup, group);
            join = static_cast<int>(s.window_tabs.tab_groups.size()) - 1;
        }
    }
    const std::wstring names = TabsLabel(s, idx);
    if (group.empty()) SetLine(r, Fill(Text(I::AiActLineUngrouped), L"{name}", names), names);
    else SetLine(r, Fill(Fill(Text(I::AiActLineGrouped), L"{name}", names), L"{group}", group), group);
    r.undo = [was](AppState& st) {
        for (const Was& w : was) {
            const int at = IndexOfTab(st, w.tab);
            if (at < 0 || w.tab->tab_group == w.group) continue;
            int pos = -1;
            for (size_t g = 0; g < st.window_tabs.tab_groups.size(); ++g)
                if (st.window_tabs.tab_groups[g].id == w.group) pos = static_cast<int>(g);
            if (w.group == 0 || pos < 0) st.tabs.RunTabCommand(st.window_tabs, at, CmdTabRemoveFromGroup);
            else st.tabs.RunTabCommand(st.window_tabs, at, CmdTabJoinGroupBase + pos);
        }
    };
    return r;
}

ArgRun TabDuplicate(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx) || idx.size() != 1) return Fail("name one tab (get_state lists them)", I::AiActWhyNoTab);
    const std::wstring name = TabName(s, idx[0]);
    if (!s.tabs.RunTabCommand(s.window_tabs, static_cast<int>(idx[0]), CmdTabDuplicate))
        return Fail("Pulse could not duplicate it", I::AiActWhyFailed);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    SetLine(r, Fill(Text(I::AiActLineDuplicated), L"{name}", name), name);
    return r;
}

ArgRun TabCloseOthers(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx) || idx.size() != 1) return Fail("name one tab (get_state lists them)", I::AiActWhyNoTab);
    size_t closable = 0;
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
        if (i != idx[0] && s.window_tabs.items[i] && !s.window_tabs.items[i]->pinned) ++closable;
    if (closable == 0) return Fail("no other tab to close (pinned tabs stay)", I::ActWhyOneTab);
    const size_t before = s.window_tabs.items.size();
    s.tabs.RunTabCommand(s.window_tabs, static_cast<int>(idx[0]), CmdTabCloseOthers);
    const size_t closed = before - s.window_tabs.items.size();
    ArgRun r;
    r.ok = true;
    r.arg = TabName(s, s.window_tabs.active);
    SetLine(r, Fill(Text(I::AiActLineClosedTabs), L"{n}", Num(closed)), Num(closed));
    // Ctrl+Shift+T brings closed tabs back, newest first.
    r.undo = [closed](AppState& st) {
        for (size_t i = 0; i < closed; ++i) ReopenLastClosed(st);
    };
    return r;
}

ArgRun TabActivate(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx) || idx.size() != 1) return Fail("name one tab (get_state lists them)", I::AiActWhyNoTab);
    ArgRun r;
    r.ok = true;
    r.arg = TabName(s, idx[0]);
    if (idx[0] == s.window_tabs.active) {
        r.skip = true;
        return r;
    }
    LayoutTab* from = s.window_tabs.Active();
    SwitchTab(s, idx[0]);
    SetLine(r, Fill(Text(I::AiActLineSwitched), L"{name}", r.arg), r.arg);
    r.undo = [from](AppState& st) {
        const int at = IndexOfTab(st, from);
        if (at >= 0) SwitchTab(st, static_cast<size_t>(at));
    };
    return r;
}

// ---- Panes and windows ----------------------------------------------------------

ArgRun PaneFocusRun(AppState& s, const json::Value& args) {
    std::vector<Pane*> panes;
    std::string error;
    if (!PanesForArg(s, args.find("pane"), panes, error) || panes.size() != 1)
        return Fail(error.empty() ? "name one pane: 1-4 or target" : error, I::AiActWhyFailed);
    ArgRun r;
    r.ok = true;
    const Tab* t = panes[0]->ActiveTab();
    r.arg = t ? (t->current_path.empty() ? L"This PC" : Leaf(t->current_path)) : std::wstring();
    if (panes[0] == s.pane) {
        r.skip = true;
        return r;
    }
    Pane* from = s.pane;
    FocusPane(s, panes[0]);
    SetLine(r, Fill(Text(I::AiActLineSwitched), L"{name}", r.arg), r.arg);
    r.undo = [from](AppState& st) {
        for (Pane* p : PanesOnScreen(LiveLayout(st)))
            if (p == from) FocusPane(st, p);
    };
    return r;
}

ArgRun WindowActivate(AppState& s, const json::Value& args) {
    const json::Value* v = args.find("window");
    std::vector<std::pair<HWND, std::wstring>> others;
    ForEachWindowSharingCore(s, [&](AppState& w) {
        if (&w == &s || !w.hwnd) return;
        const LayoutTab* lt = w.window_tabs.Active();
        others.push_back({w.hwnd, lt ? TabStripTitle(*lt) : std::wstring()});
    });
    if (others.empty()) return Fail("there is only one window", I::AiActWhyOneWindow);
    HWND pick = nullptr;
    std::wstring name;
    int64_t k = 0;
    if (!v && others.size() == 1) {
        pick = others[0].first;
        name = others[0].second;
    } else if (v && v->get_int(k, 1, static_cast<int64_t>(others.size()))) {
        pick = others[static_cast<size_t>(k - 1)].first;
        name = others[static_cast<size_t>(k - 1)].second;
    } else if (v && v->is_string()) {
        const std::wstring want = Trim(W(v->as_string()));
        for (const auto& [h, title] : others)
            if (!pick && (SameText(title, want) || (!want.empty() && title.find(want) != std::wstring::npos))) {
                pick = h;
                name = title;
            }
    }
    if (!pick) return Fail("no such window (get_state other_windows)", I::AiActWhyNoWindow);
    if (IsIconic(pick)) ShowWindow(pick, SW_RESTORE);
    SetForegroundWindow(pick);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    SetLine(r, Fill(Text(I::AiActLineSwitched), L"{name}", name), name);
    return r;
}

// ---- Sidebar and view -------------------------------------------------------------

struct SectionWord {
    std::string_view word;
    SidebarSectionId id;
    I name;
};
constexpr SectionWord kSections[] = {
    {"workspaces", SidebarSectionId::Workspaces, I::SidebarWorkspaces},
    {"quick_access", SidebarSectionId::QuickAccess, I::SidebarQuickAccess},
    {"saved_searches", SidebarSectionId::SavedSearches, I::SidebarSavedSearches},
    {"drives", SidebarSectionId::Drives, I::SidebarDrives},
    {"tags", SidebarSectionId::Tags, I::SidebarTags},
    {"networks", SidebarSectionId::Networks, I::SidebarNetworkLocations},
};

ArgRun SidebarSection(AppState& s, const json::Value& args) {
    const std::string want = args.get_string("section");
    const SectionWord* sec = nullptr;
    for (const SectionWord& w : kSections)
        if (w.word == want || W(want) == Text(w.name)) sec = &w;
    if (!sec) return Fail("section: workspaces|quick_access|saved_searches|drives|tags|networks", I::AiActWhyFailed);
    const int id = static_cast<int>(sec->id);
    const bool open_now = ((s.sidebarCollapsedMask >> id) & 1u) == 0;
    const bool open = args.get_bool("open", !open_now);
    ArgRun r;
    r.ok = true;
    r.arg = Text(sec->name);
    if (open == open_now) {
        r.skip = true;
        return r;
    }
    ToggleSidebarSection(s, id);
    const std::wstring from = Text(open_now ? I::AiActExpanded : I::AiActFolded);
    const std::wstring to = Text(open ? I::AiActExpanded : I::AiActFolded);
    SetLine(r, Fill(Fill(Fill(Text(I::AiActLineSection), L"{name}", r.arg), L"{from}", from), L"{to}", to), to);
    r.undo = [id, open](AppState& st) {
        if ((((st.sidebarCollapsedMask >> id) & 1u) == 0) == open) ToggleSidebarSection(st, id);
    };
    return r;
}

ArgRun IconSize(AppState& s, const json::Value& args) {
    Tab* tab = ActiveTab(s);
    if (!tab) return Fail("no folder is open", I::ActWhyNotFolder);
    const json::Value* v = args.find("size");
    const float old = tab->icon_dip;
    int step = 0;
    if (v && v->is_string() && (v->as_string() == "bigger" || v->as_string() == "larger")) step = 1;
    else if (v && v->is_string() && v->as_string() == "smaller") step = -1;
    ArgRun r;
    if (step != 0) {
        ZoomIconSize(s, step);
        r.arg = Text(step > 0 ? I::ActIconBigger : I::ActIconSmaller);
        r.undo = [step](AppState& st) { ZoomIconSize(st, -step); };
    } else if (v && v->is_number() && v->as_number() >= 16 && v->as_number() <= 512) {
        SetIconSize(s, static_cast<float>(v->as_number()));
        r.arg = std::to_wstring(static_cast<int>(v->as_number())) + L" px";
        if (old > 0) r.undo = [old](AppState& st) { SetIconSize(st, old); };
    } else {
        return Fail("size: bigger|smaller|16-512", I::AiActWhyFailed);
    }
    r.ok = true;
    SetLine(r, Text(I::ActIconSize) + L" \u00B7 " + r.arg, r.arg);
    return r;
}

// ---- Tags -------------------------------------------------------------------------

const ColorTag* TagNamed(AppState& s, const std::wstring& name) {
    for (const ColorTag& t : s.places.tags)
        if (SameText(t.name, name)) return &t;
    const TagId id = s.places.ResolveTagRef(name);
    return id.empty() ? nullptr : s.places.FindTag(id);
}

// The tag group named `name`, made when there is none ("" for none).
std::wstring GroupNamed(AppState& s, const std::wstring& name, bool make) {
    if (name.empty()) return {};
    for (const app::TagGroup& g : s.places.tag_groups)
        if (SameText(g.name, name)) return g.id;
    return make ? s.places.CreateTagGroup(name) : std::wstring();
}

std::wstring GroupName(AppState& s, const std::wstring& id) {
    const app::TagGroup* g = s.places.FindTagGroup(id);
    return g ? g->name : std::wstring();
}

void TagAds(AppState& s, const std::vector<std::wstring>& paths) {
    if (!paths.empty()) QueueTagAds(s, BuildTagAdsUpdates(s.places, paths));
    RefreshSidebarModel(s);
    Redraw(s);
}

ArgRun TagCreate(AppState& s, const json::Value& args) {
    const std::wstring name = Trim(W(args.get_string("name")));
    if (name.empty()) return Fail("name is missing", I::AiActWhyNoName);
    if (TagNamed(s, name)) return Fail("a tag with this name exists (tag.edit changes it)", I::AiActWhyTagExists);
    const std::string color_word = args.get_string("color");
    const int color = color_word.empty() ? -1 : ColorIndex(color_word);
    if (color == -2) return Fail("color: red|orange|yellow|green|teal|blue|purple|pink", I::AiActWhyFailed);
    const std::vector<uint32_t>& palette = TagColorPalette(s);
    uint32_t rgb = palette.empty() ? 0xEF4444 : palette[s.places.tags.size() % palette.size()];
    if (color >= 0) rgb = NearestTagColor(s, TabController::Palette()[color]);
    const std::wstring group_name = Trim(W(args.get_string("group")));
    const bool group_existed = !GroupNamed(s, group_name, false).empty();
    const TagId id = s.places.CreateTag(name, rgb);
    if (id.empty()) return Fail("Pulse could not make the tag", I::AiActWhyFailed);
    std::wstring group;
    if (!group_name.empty()) {
        group = GroupNamed(s, group_name, true);
        if (!group.empty()) s.places.SetTagGroup(id, group);
    }
    TagAds(s, {});
    ArgRun r;
    r.ok = true;
    r.arg = name;
    r.sub = group.empty() ? TagColorName(rgb)
                          : Fill(Fill(Text(I::AiActTagIn), L"{color}", TagColorName(rgb)), L"{group}", group_name);
    SetLine(r, Fill(Text(I::AiActLineTagNew), L"{name}", name), name);
    const std::wstring made_group = group_existed ? std::wstring() : group;
    r.undo = [id, made_group](AppState& st) {
        const ColorTag* t = st.places.FindTag(id);
        if (!t) return;
        const std::vector<std::wstring> paths = t->paths;
        if (paths.empty()) DeleteTagAndRefreshViews(st, id);
        if (!made_group.empty()) {
            bool used = false;
            for (const ColorTag& other : st.places.tags) used = used || other.group == made_group;
            if (!used) st.places.DeleteTagGroup(made_group);
        }
        TagAds(st, {});
    };
    return r;
}

ArgRun TagEdit(AppState& s, const json::Value& args) {
    const ColorTag* tag = TagNamed(s, Trim(W(args.get_string("tag"))));
    if (!tag) return Fail("no such tag (find_actions tag.toggle lists them)", I::AiActWhyNoTagNamed);
    const TagId id = tag->id;
    const std::wstring old_name = tag->name;
    const uint32_t old_rgb = tag->rgb;
    const std::wstring name = Trim(W(args.get_string("name")));
    const std::string color_word = args.get_string("color");
    const int color = color_word.empty() ? -1 : ColorIndex(color_word);
    if (color == -2) return Fail("color: red|orange|yellow|green|teal|blue|purple|pink", I::AiActWhyFailed);
    if (name.empty() && color < 0) return Fail("give name or color", I::AiActWhyNoName);
    if (!name.empty() && !SameText(name, old_name) && TagNamed(s, name))
        return Fail("a tag with this name exists", I::AiActWhyTagExists);
    const std::vector<std::wstring> affected = s.places.PathsForTag(id);
    ArgRun r;
    r.ok = true;
    r.arg = old_name;
    std::vector<std::wstring> parts;
    if (!name.empty() && name != old_name) {
        s.places.RenameTag(id, name);
        parts.push_back(Fill(Fill(Text(I::AiActLineRenamed), L"{from}", old_name), L"{to}", name));
    }
    if (color >= 0) {
        const uint32_t rgb = NearestTagColor(s, TabController::Palette()[color]);
        if (rgb != old_rgb) {
            s.places.SetTagColor(id, rgb);
            parts.push_back(TagColorName(old_rgb) + L" \u2192 " + TagColorName(rgb));
        }
    }
    if (parts.empty()) {
        r.skip = true;
        return r;
    }
    TagAds(s, affected);
    std::wstring line;
    for (const auto& p : parts) line += (line.empty() ? L"" : L"\uFF0C") + p;
    SetLine(r, line, name.empty() ? std::wstring() : name);
    r.undo = [id, old_name, old_rgb](AppState& st) {
        if (!st.places.FindTag(id)) return;
        const std::vector<std::wstring> paths = st.places.PathsForTag(id);
        st.places.RenameTag(id, old_name);
        st.places.SetTagColor(id, old_rgb);
        TagAds(st, paths);
    };
    return r;
}

// Deleting keeps everything the tag had, so 撤销 puts it all back (P0).
ArgRun TagDelete(AppState& s, const json::Value& args) {
    const ColorTag* tag = TagNamed(s, Trim(W(args.get_string("tag"))));
    if (!tag) return Fail("no such tag (find_actions tag.toggle lists them)", I::AiActWhyNoTagNamed);
    const TagId id = tag->id;
    const std::wstring name = tag->name, group = tag->group;
    const uint32_t rgb = tag->rgb;
    const int favorite = tag->favorite;
    const std::vector<std::wstring> paths = s.places.PathsForTag(id);
    if (!DeleteTagAndRefreshViews(s, id)) return Fail("Pulse could not delete the tag", I::AiActWhyFailed);
    RefreshSidebarModel(s);
    Redraw(s);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    SetLine(r, Fill(Fill(Text(I::AiActLineTagDeleted), L"{name}", name), L"{n}", Num(paths.size())), name);
    r.undo = [name, rgb, group, favorite, paths](AppState& st) {
        if (TagNamed(st, name)) return;
        const TagId again = st.places.CreateTag(name, rgb);
        if (again.empty()) return;
        if (!group.empty() && st.places.FindTagGroup(group)) st.places.SetTagGroup(again, group);
        if (favorite > 0) st.places.SetTagFavorite(again, true);
        if (!paths.empty()) st.places.SetTagsBatch(again, paths, true);
        TagAds(st, paths);
    };
    return r;
}

ArgRun TagGroupRun(AppState& s, const json::Value& args) {
    const ColorTag* tag = TagNamed(s, Trim(W(args.get_string("tag"))));
    if (!tag) return Fail("no such tag", I::AiActWhyNoTagNamed);
    const TagId id = tag->id;
    const std::wstring name = tag->name, old_group = tag->group;
    const std::wstring group_name = Trim(W(args.get_string("group")));
    const bool existed = group_name.empty() || !GroupNamed(s, group_name, false).empty();
    const std::wstring group = GroupNamed(s, group_name, true);
    if (!group_name.empty() && group.empty()) return Fail("bad group name", I::AiActWhyFailed);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    if (group == old_group) {
        r.skip = true;
        return r;
    }
    s.places.SetTagGroup(id, group);
    TagAds(s, {});
    if (group.empty()) SetLine(r, Fill(Text(I::AiActLineUngrouped), L"{name}", name), name);
    else SetLine(r, Fill(Fill(Text(I::AiActLineGrouped), L"{name}", name), L"{group}", group_name), group_name);
    const std::wstring made = existed ? std::wstring() : group;
    r.undo = [id, old_group, group, made](AppState& st) {
        const ColorTag* t = st.places.FindTag(id);
        if (!t || t->group != group) return;
        st.places.SetTagGroup(id, st.places.FindTagGroup(old_group) ? old_group : std::wstring());
        if (!made.empty()) st.places.DeleteTagGroup(made);
        TagAds(st, {});
    };
    return r;
}

ArgRun SmartViewSave(AppState& s, const json::Value& args) {
    const Tab* tab = ActiveTab(s);
    TagQuery query;
    if (!tab || !TagQueryFromPath(s.places, tab->current_path, &query))
        return Fail("open a tag view first (tags combined with 且/或)", I::ActWhyNotFolder);
    std::wstring name = Trim(W(args.get_string("name")));
    if (name.empty()) name = TagQueryTitle(s.places, query);
    for (const SavedSearch& item : s.savedSearches.items())
        if (SameText(item.name, name)) return Fail("a saved view has this name", I::AiActWhyTagExists);
    SavedSearch saved;
    saved.name = name;
    saved.mode = SavedSearchMode::Tags;
    saved.root = query.in.empty() ? std::wstring(L"pulse:tags") : query.in;
    saved.query = MakeTagQueryPath(query);
    if (!s.savedSearches.Add(std::move(saved))) return Fail("the saved list is full", I::AiActWhyFailed);
    s.savedSearches.Save();
    SyncSavedSearchSidebar(s);
    Redraw(s);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    SetLine(r, Text(I::ActSmartViewSave) + L" \u00B7 " + name, name);
    r.undo = [name](AppState& st) {
        const auto& items = st.savedSearches.items();
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].name == name) {
                st.savedSearches.Remove(i);
                st.savedSearches.Save();
                SyncSavedSearchSidebar(st);
                Redraw(st);
                return;
            }
    };
    return r;
}

// ---- Items: recent, aliases, tray ---------------------------------------------------

std::vector<std::wstring> PathsArg(AppState& s, const json::Value& args, const char* key) {
    std::vector<std::wstring> out;
    if (const json::Value* v = args.find(key)) {
        if (v->is_string()) out.push_back(W(v->as_string()));
        else if (v->is_array())
            for (size_t i = 0; i < v->size(); ++i)
                if (v->at(i).is_string()) out.push_back(W(v->at(i).as_string()));
    }
    if (out.empty())
        if (const Tab* tab = ActiveTab(s)) out = SelectedFullPaths(*tab);
    return out;
}

std::vector<std::wstring> Leaves(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> out;
    for (const auto& p : paths) out.push_back(Leaf(p));
    return out;
}

ArgRun RecentRemove(AppState& s, const json::Value& args) {
    std::vector<std::pair<std::wstring, PlaceItemKind>> removed;
    for (const std::wstring& p : PathsArg(s, args, "items"))
        if (const RecentItem* item = s.places.FindRecent(p)) {
            const PlaceItemKind kind = item->kind;
            const std::wstring path = item->path;
            if (s.places.RemoveRecent(path)) removed.push_back({path, kind});
        }
    if (removed.empty()) return Fail("none of these are in Recent", I::AiActWhyNoItems);
    Redraw(s);
    std::vector<std::wstring> paths;
    for (const auto& [p, _] : removed) paths.push_back(p);
    ArgRun r;
    r.ok = true;
    r.arg = NameList(Leaves(paths));
    SetLine(r, Fill(Text(I::AiActLineRecentRemoved), L"{items}", r.arg), r.arg);
    r.undo = [removed](AppState& st) {
        for (const auto& [p, kind] : removed)
            if (!st.places.FindRecent(p)) st.places.RecordRecent(p, kind);
        Redraw(st);
    };
    return r;
}

ArgRun AliasSet(AppState& s, const json::Value& args) {
    std::vector<std::wstring> paths = PathsArg(s, args, "path");
    if (paths.size() != 1) return Fail("give one path (or select one item)", I::ActWhySelectOne);
    const std::wstring path = paths[0];
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return Fail("no such file or folder", I::AiActWhyNoFile);
    const std::wstring alias = Trim(W(args.get_string("alias")));
    AliasStore& store = AliasStore::Instance();
    const std::wstring old = store.Find(path);
    ArgRun r;
    r.ok = true;
    r.arg = Leaf(path);
    if (old == alias) {
        r.skip = true;
        return r;
    }
    if (!store.Set(path, alias)) return Fail("Pulse could not set the alias", I::AiActWhyFailed);
    Redraw(s);
    if (alias.empty()) SetLine(r, Fill(Text(I::AiActLineAliasOff), L"{name}", r.arg), r.arg);
    else SetLine(r, Fill(Fill(Text(I::AiActLineAlias), L"{name}", r.arg), L"{alias}", alias), alias);
    r.undo = [path, alias, old](AppState& st) {
        AliasStore& a = AliasStore::Instance();
        if (a.Find(path) == alias) a.Set(path, old);
        Redraw(st);
    };
    return r;
}

ArgRun TrayRemove(AppState& s, const json::Value& args) {
    const json::Value* v = args.find("items");
    const bool all = !v || (v->is_string() && v->as_string() == "all");
    std::vector<std::wstring> want;
    if (!all) want = PathsArg(s, args, "items");
    std::vector<uint64_t> ids;
    std::vector<std::pair<std::wstring, bool>> gone;  // path, move intent
    for (const TrayBatch& batch : s.tray.batches())
        for (const TrayItem& item : batch.items) {
            bool hit = all;
            for (const auto& w : want) hit = hit || SameText(w, item.path) || SameText(w, Leaf(item.path));
            if (!hit || item.inflight_task) continue;
            ids.push_back(item.id);
            gone.push_back({item.path, batch.move_intent});
        }
    if (ids.empty()) return Fail(all ? "the tray is empty" : "none of these are on the tray", all ? I::ActWhyNoTray : I::AiActWhyNoItems);
    RemoveTrayItems(s, ids);
    std::vector<std::wstring> paths;
    for (const auto& [p, _] : gone) paths.push_back(p);
    ArgRun r;
    r.ok = true;
    r.arg = NameList(Leaves(paths));
    SetLine(r, Fill(Text(I::AiActLineTrayRemoved), L"{items}", r.arg), r.arg);
    r.undo = [gone](AppState& st) {
        std::vector<std::wstring> copy, move;
        for (const auto& [p, m] : gone) (m ? move : copy).push_back(p);
        if (!copy.empty()) CollectPathsToTray(st, copy, false);
        if (!move.empty()) CollectPathsToTray(st, move, true);
    };
    return r;
}

// ---- Search -----------------------------------------------------------------------

ArgRun SearchOpen(AppState& s, const json::Value& args) {
    const std::wstring query = Trim(W(args.get_string("query")));
    if (query.empty()) return Fail("query is missing", I::AiActWhyNoName);
    const Tab* tab = ActiveTab(s);
    const std::wstring before = tab ? tab->current_path : std::wstring();
    const std::wstring path = MakeSearchPath(query);
    NavigateTo(s, path);
    ArgRun r;
    r.ok = true;
    r.arg = query;
    SetLine(r, Fill(Text(I::AiActLineSearch), L"{query}", query), query);
    r.undo = [path](AppState& st) {
        const Tab* t = ActiveTab(st);
        if (t && t->current_path == path) GoBack(st);
    };
    return r;
}

ArgRun IndexStatus(AppState& s, const json::Value&) {
    ArgRun r;
    r.ok = true;
    r.read_only = true;
    r.sub = ContentIndexStatusText(s);
    r.data = json::Value::Object();
    r.data.set("status", U8(r.sub));
    return r;
}

// ---- Network ------------------------------------------------------------------------

ArgRun NetPlace(AppState& s, const json::Value& args) {
    std::wstring unc = Trim(W(args.get_string("path")));
    if (unc.empty())
        if (const Tab* tab = ActiveTab(s); tab && tab->current_path.rfind(L"\\\\", 0) == 0) unc = tab->current_path;
    if (unc.rfind(L"\\\\", 0) != 0) return Fail("path: a \\\\server\\share path", I::AiActWhyNoFolder);
    const bool pin = args.get_bool("pin", true);
    const std::wstring name = Trim(W(args.get_string("name")));
    const bool pinned = s.places.FindNetwork(unc) >= 0;
    ArgRun r;
    r.ok = true;
    r.arg = name.empty() ? Leaf(unc) : name;
    if (!pin) {
        if (!pinned) {
            r.skip = true;
            return r;
        }
        s.places.UnpinNetwork(unc);
        SetLine(r, Fill(Text(I::AiActLineNetUnpinned), L"{name}", r.arg), r.arg);
        r.undo = [unc, name](AppState& st) {
            if (st.places.FindNetwork(unc) < 0) st.places.PinNetwork(unc, name);
            RefreshSidebarModel(st);
            Redraw(st);
        };
    } else if (!pinned) {
        s.places.PinNetwork(unc, name);
        SetLine(r, Fill(Text(I::AiActLineNetPinned), L"{name}", r.arg), r.arg);
        r.undo = [unc](AppState& st) {
            st.places.UnpinNetwork(unc);
            RefreshSidebarModel(st);
            Redraw(st);
        };
    } else if (!name.empty()) {
        s.places.RenameNetwork(unc, name);
        SetLine(r, Fill(Fill(Text(I::AiActLineRenamed), L"{from}", Leaf(unc)), L"{to}", name), name);
    } else {
        r.skip = true;
        return r;
    }
    RefreshSidebarModel(s);
    Redraw(s);
    return r;
}

// ---- Keymap -------------------------------------------------------------------------

// "action": an action table id (file.new_folder), a keymap id (NewFolder) or
// the name the settings page shows (新建文件夹).
KeyAction KeyActionArg(const json::Value& args) {
    const std::string want = args.get_string("action");
    if (want.empty()) return KeyAction::Count;
    for (const KeyActionLink& link : kKeyActionLinks)
        if (link.link.id == want) return link.key;
    const KeyAction by_id = ActionFromId(want);
    if (by_id != KeyAction::Count) return by_id;
    const std::wstring name = W(want);
    for (int i = 0; i < kKeyActionCount; ++i)
        if (SameText(ActionName(static_cast<KeyAction>(i)), name)) return static_cast<KeyAction>(i);
    return KeyAction::Count;
}

std::wstring ChordsText(const std::vector<KeyChord>& chords) {
    std::wstring out;
    for (const KeyChord& c : chords) out += (out.empty() ? L"" : L" / ") + ChordDisplay(c);
    return out;
}

ArgRun KeymapRead(AppState&, const json::Value& args) {
    const Keymap& km = ActiveKeymap();
    const KeyAction only = KeyActionArg(args);
    json::Value list = json::Value::Array();
    for (int i = 0; i < kKeyActionCount; ++i) {
        const KeyAction a = static_cast<KeyAction>(i);
        if (only != KeyAction::Count && a != only) continue;
        json::Value row = json::Value::Object();
        row.set("action", ActionInfo(a).id);
        row.set("name", U8(ActionName(a)));
        row.set("keys", U8(ChordsText(km.EffectiveChords(a))));
        if (ActionInfo(a).kind != KeyKind::Normal) row.set("fixed", true);
        if (!km.IsDefault(a)) row.set("changed", true);
        list.push(std::move(row));
    }
    ArgRun r;
    r.ok = true;
    r.read_only = true;
    r.data = json::Value::Object();
    r.data.set("shortcuts", std::move(list));
    if (only != KeyAction::Count) r.sub = ChordsText(km.EffectiveChords(only));
    return r;
}

void ApplyKeymap(AppState& s, const Keymap& km) {
    s.appPrefs.shortcuts = km.ToPrefs();
    if (!s.isolatedTest) s.appPrefs.Save();
    ApplyKeymapPrefs(s);
}

// Another action's key is never taken over: the step fails and says whose
// it is, so the assistant asks the user first.
ArgRun KeymapSet(AppState& s, const json::Value& args) {
    const KeyAction action = KeyActionArg(args);
    if (action == KeyAction::Count) return Fail("unknown action (keymap.read lists them)", I::AiActWhyUnknown);
    const std::wstring title = ActionName(action);
    if (ActionInfo(action).kind != KeyKind::Normal) return Fail("this shortcut cannot be changed", I::AiActWhyKeyFixed);
    KeyChord chord;
    if (!ParseChord(W(args.get_string("keys")), chord) || chord.empty())
        return Fail("keys: like Ctrl+Shift+F", I::AiActWhyKeyBad);
    Keymap km = ActiveKeymap();
    // What the settings recorder checks: the Chinese IME, and one
    // RegisterHotKey for the new chord (another program may hold it).
    ShortcutEnv env;
    env.chinese_ime = ChineseImeInstalled();
    const bool ours = km.GlobalHotkeyEnabled() && chord == km.GlobalHotkey();
    if (!ours && (chord.mods & (kKeyCtrl | kKeyAlt | kKeyWin)) && !ProbeTakenChords({chord}).empty())
        env.taken.push_back(chord);
    const ChordVerdict verdict = CheckChord(km, action, chord, env);
    ArgRun r;
    r.arg = title;
    if (verdict.issue == ChordIssue::Same) {
        r.ok = true;
        r.skip = true;
        return r;
    }
    if (verdict.issue == ChordIssue::Duplicate || verdict.issue == ChordIssue::DuplicateFixed) {
        const std::wstring other = verdict.other.found() ? ActionName(verdict.other.action) : std::wstring();
        r.error = "conflict: " + U8(ChordDisplay(chord)) + " is the key of " + U8(other) +
                  "; nothing changed. Ask the user before changing that one.";
        r.why = Fill(Fill(Text(I::AiActWhyKeyTaken), L"{keys}", ChordDisplay(chord)), L"{name}", other);
        r.data = json::Value::Object();
        r.data.set("conflict_with", U8(other));
        return r;
    }
    if (verdict.issue == ChordIssue::Taken || verdict.issue == ChordIssue::Ime) {
        // It would save, but may not work: ask the user (P4 decision 2).
        const bool held = verdict.issue == ChordIssue::Taken;
        json::Value spare = json::Value::Array();
        std::string offer;
        for (const KeyChord& k : SuggestChords(km, action, env, 3)) {
            spare.push(U8(ChordDisplay(k)));
            offer += (offer.empty() ? "" : ", ") + U8(ChordDisplay(k));
        }
        r.error = U8(ChordDisplay(chord)) +
                  (held ? " is held by another program" : " is used by the Chinese input method") +
                  " and may not work in Pulse; nothing changed. Ask the user" +
                  (offer.empty() ? std::string(".") : " - free keys: " + offer + ".");
        r.why = Fill(Text(held ? I::AiActWhyKeyHeld : I::AiActWhyKeyIme), L"{keys}", ChordDisplay(chord));
        r.data = json::Value::Object();
        r.data.set("suggest", std::move(spare));
        return r;
    }
    if (!verdict.can_save()) return Fail("this key combination cannot be used", I::AiActWhyKeyBad);
    const std::vector<KeyChord> old = km.Bindings(action);
    km.SetBindings(action, {chord});
    ApplyKeymap(s, km);
    r.ok = true;
    r.sub = Fill(Text(I::AiActKeyFree), L"{keys}", ChordDisplay(chord));
    r.line_before = title + L" ";
    if (!old.empty()) r.caps_from = ChordCaps(old.front());
    r.caps_to = ChordCaps(chord);
    r.data = json::Value::Object();
    r.data.set("from", U8(ChordsText(old)));
    r.data.set("to", U8(ChordDisplay(chord)));
    r.undo = [action, old, chord](AppState& st) {
        Keymap now = ActiveKeymap();
        const auto& b = now.Bindings(action);
        if (b.size() != 1 || b.front() != chord) return;
        now.SetBindings(action, old);
        ApplyKeymap(st, now);
    };
    return r;
}

ArgRun WindowArrangeRun(AppState& s, const json::Value& args) {
    const auto* value=args.find("layout");
    if (!value || !value->is_string()) return Fail("layout is required", I::ArrangeUnavailable);
    constexpr std::string_view names[]={"side_by_side","stacked","columns","tile","main","cascade","main_right","main_top"};
    for (int i=0;i<8;++i) if (value->as_string()==names[i]) {
        const auto layout=static_cast<WindowArrangeLayout>(i);
        if (!ArrangeWindows(s,layout)) return Fail("windows cannot be arranged now",I::ArrangeUnavailable);
        ArgRun result; result.ok=true; result.title=Text(WindowArrangeLabel(layout));
        result.line_before=Text(I::ArrangeApplied); result.undo=[token=WindowArrangeGeneration()](AppState& st){UndoWindowArrange(st,token);};
        return result;
    }
    return Fail("unknown layout",I::ArrangeUnavailable);
}

// ---- The table ------------------------------------------------------------------------

using Runner = ArgRun (*)(AppState&, const json::Value&);

constexpr std::pair<std::string_view, Runner> kRunners[] = {
    {"tab.pin", TabPin},
    {"tab.color", TabColor},
    {"tab.rename", TabRename},
    {"tab.group", TabGroupRun},
    {"tab.duplicate", TabDuplicate},
    {"tab.close_others", TabCloseOthers},
    {"tab.activate", TabActivate},
    {"pane.focus", PaneFocusRun},
    {"window.activate", WindowActivate},
    {"window.arrange", WindowArrangeRun},
    {"sidebar.section", SidebarSection},
    {"view.icon_size", IconSize},
    {"tag.create", TagCreate},
    {"tag.edit", TagEdit},
    {"tag.delete", TagDelete},
    {"tag.group", TagGroupRun},
    {"smartview.save", SmartViewSave},
    {"recent.remove", RecentRemove},
    {"alias.set", AliasSet},
    {"tray.remove", TrayRemove},
    {"search.open", SearchOpen},
    {"index.status", IndexStatus},
    {"net.place", NetPlace},
    {"keymap.read", KeymapRead},
    {"keymap.set", KeymapSet},
};

constexpr bool RunnersKnown() {
    for (const auto& [id, _] : kRunners)
        if (!FindAction(id)) return false;
    return true;
}
static_assert(RunnersKnown(), "kRunners: an id is missing from action_inventory.h");

} // namespace

int ColorIndex(std::string_view word) noexcept {
    if (word == "none" || word.empty()) return -1;
    for (size_t i = 0; i < std::size(kColorWords); ++i)
        if (kColorWords[i] == word) return static_cast<int>(i);
    if (word == "cyan") return 4;
    if (word == "violet") return 6;
    return -2;
}

std::wstring ColorName(int index) {
    if (index < 0 || static_cast<size_t>(index) >= std::size(kColorNames)) return Text(I::ActColorNone);
    return Text(kColorNames[index]);
}

bool HasArgRunner(std::string_view id) noexcept {
    for (const auto& [k, _] : kRunners)
        if (k == id) return true;
    return HasExtRunner(id);
}

ArgRun RunWithArgs(AppState& s, std::string_view id, const json::Value& args) {
    for (const auto& [k, run] : kRunners)
        if (k == id) return run(s, args);
    if (HasExtRunner(id)) return RunExt(s, id, args);
    return Fail("unknown action", I::AiActWhyUnknown);
}

} // namespace pulse::app::actions
