// ai_agent_actions.cpp - Pulse 助手's run_actions' steps: panes, windows, view, selection, tags and the rest.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

const wchar_t* PresetName(app::LayoutPreset p) {
    switch (p) {
    case app::LayoutPreset::Single: return l10n::Get(I::AiCtlLayoutSingle).c_str();
    case app::LayoutPreset::TwoVertical: return l10n::Get(I::AiCtlLayoutTwoV).c_str();
    case app::LayoutPreset::TwoHorizontal: return l10n::Get(I::AiCtlLayoutTwoH).c_str();
    case app::LayoutPreset::Three: return l10n::Get(I::AiCtlLayoutThree).c_str();
    case app::LayoutPreset::FourGrid: return l10n::Get(I::AiCtlLayoutFour).c_str();
    default: return l10n::Get(I::AiCtlLayoutCustom).c_str();
    }
}

// The panes in screen order, numbered as get_state numbers them.
std::vector<app::Pane*> ShownPanes(AppState& s) {
    if (!Root(s)) return {};
    return app::actions::PanesOnScreen(LiveLayout(s));
}

bool PaneAlive(AppState& s, const app::Pane* p) {
    if (!p) return false;
    for (const auto& owned : Panes(s))
        if (owned.get() == p) return true;
    return false;
}

// 左栏 / 右栏, 上栏 / 下栏, 第 3 栏; empty with one pane.
std::wstring PanePos(AppState& s, size_t i) {
    const size_t n = ShownPanes(s).size();
    if (n < 2) return {};
    const app::LayoutPreset p = LayoutOf(s);
    if (n == 2 && p == app::LayoutPreset::TwoVertical) return Text(i == 0 ? I::AiCtlPaneLeft : I::AiCtlPaneRight);
    if (n == 2 && p == app::LayoutPreset::TwoHorizontal) return Text(i == 0 ? I::AiCtlPaneTop : I::AiCtlPaneBottom);
    return Fill(Text(I::AiCtlPaneN), L"{n}", Num(i + 1));
}

size_t PaneIndex(AppState& s, const app::Pane* p) {
    const auto v = ShownPanes(s);
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == p) return i;
    return 0;
}

std::wstring PlaceLabel(const std::wstring& path) {
    if (path.empty()) return Text(I::AiCtlThisPc);
    return FolderLabel(path);
}

// 左栏 下载 · 右栏 桌面
std::wstring PanesLine(AppState& s) {
    std::wstring out;
    const auto v = ShownPanes(s);
    for (size_t i = 0; i < v.size(); ++i) {
        const app::Tab* t = v[i]->ActiveTab();
        if (!out.empty()) out += L" \u00B7 ";
        out += PanePos(s, i) + L" " + PlaceLabel(t ? t->current_path : std::wstring());
    }
    return out;
}

// The keys now bound to `action` (a range action: its `position`th chord).
std::vector<std::wstring> KeysFor(app::KeyAction action, int position) {
    const app::Keymap& km = app::ActiveKeymap();
    if (position >= 0) {
        const auto chords = km.RangeChords(action, km.RangeMods(action));
        if (static_cast<size_t>(position) < chords.size()) return app::ChordCaps(chords[static_cast<size_t>(position)]);
        return {};
    }
    const auto& chords = km.Bindings(action);
    return chords.empty() ? std::vector<std::wstring>() : app::ChordCaps(chords.front());
}

void AddKeys(CtlRun& run, std::wstring label, std::vector<std::wstring> caps) {
    if (caps.empty() || run.keys.size() >= 4) return;
    for (const auto& k : run.keys)
        if (k.label == label) return;
    run.keys.push_back({std::move(label), std::move(caps)});
}

// A folder the model named: absolute, a drive (D:), or a known folder.
std::wstring CtlFolder(const std::string& utf8) {
    std::wstring p = ResolveTaskDest(json::Wide(utf8));
    if (p.empty() || !IsDirectory(p)) return {};
    return p;
}

std::vector<std::wstring> CtlItems(AppState& s, const json::Value& act) {
    std::vector<std::wstring> out;
    if (const json::Value* v = act.find("items"); v && v->is_array()) {
        for (size_t i = 0; i < v->size() && out.size() < 500; ++i) {
            if (!v->at(i).is_string()) continue;
            std::wstring p = json::Wide(v->at(i).as_string());
            if (p.empty() || fs::IsVirtualPath(p)) continue;
            if (const std::wstring known = ResolveTaskDest(p); !known.empty()) p = known;
            if (Exists(p)) out.push_back(NormDir(p));
        }
        return out;
    }
    app::Tab* tab = ActiveTab(s);
    if (tab && tab->snapshot && !fs::IsVirtualPath(tab->current_path)) out = SelectedFullPaths(*tab);
    return out;
}

// `fn` after `delay` ms: on the chat's queue for the assistant, on a timer of
// its own for an MCP run. A result >= 0 runs it again after that many ms.
void Later(AppState& s, const CtlRun& run, uint32_t delay, std::function<int(AppState&)> fn) {
    if (run.mcp) McpLater(s, run.mcp, delay, std::move(fn));
    else AiChatPush(s, delay, std::move(fn));
}

// Runs `fn` with `p` focused, then gives the focus back.
template <class Fn>
void WithPane(AppState& s, app::Pane* p, Fn&& fn) {
    app::Pane* before = s.pane;
    if (p && p != s.pane) FocusPane(s, p);
    fn();
    if (before && before != s.pane && PaneAlive(s, before)) FocusPane(s, before);
}

// The windows of one call cascade from each other instead of all opening on
// the same spot next to this one.
HWND CtlOpenWindow(AppState& s, CtlRun& run, WindowInit init) {
    RECT last{};
    MONITORINFO mi{sizeof(mi)};
    if (run.last_window && IsWindow(run.last_window) && !IsZoomed(run.last_window) && !IsIconic(run.last_window) &&
        GetWindowRect(run.last_window, &last) &&
        GetMonitorInfoW(MonitorFromWindow(run.last_window, MONITOR_DEFAULTTONEAREST), &mi))
        init.rect = app::CascadeWindowRect(last, mi.rcWork, static_cast<int>(std::lround(32.0f * (s.scale > 0.0f ? s.scale : 1.0f))));
    const HWND h = OpenPulseWindow(s, init);
    if (h) run.last_window = h;
    return h;
}

CtlStep CtlLayout(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Split;
    st.title = Text(I::AiCtlStepLayout);
    // get_state's names; control_pulse's old ones still work.
    static const struct { const char* id; app::LayoutPreset p; int key; } kMap[] = {
        {"single", app::LayoutPreset::Single, 0},       {"side_by_side", app::LayoutPreset::TwoVertical, 1},
        {"stacked", app::LayoutPreset::TwoHorizontal, -1}, {"three", app::LayoutPreset::Three, 2},
        {"four", app::LayoutPreset::FourGrid, 3},       {"two_vertical", app::LayoutPreset::TwoVertical, 1},
        {"two_horizontal", app::LayoutPreset::TwoHorizontal, -1},
    };
    const std::string want = act.get_string("layout");
    const auto* m = std::find_if(std::begin(kMap), std::end(kMap), [&](const auto& e) { return want == e.id; });
    if (m == std::end(kMap)) {
        st.error = "unknown layout";
        return st;
    }
    st.arg = PresetName(m->p);
    const app::LayoutPreset before = LayoutOf(s);
    app::Pane* focus = s.pane;
    const app::LayoutTab* lt = s.window_tabs.Active();
    if (before != m->p) {
        ApplyLayoutPreset(s, m->p);
        if (LayoutOf(s) != m->p) {
            st.error = "this tab cannot be split (a settings page)";
            st.why = Text(I::AiActWhyFailed);
            return st;
        }
        if (before != app::LayoutPreset::Custom)
            run.undo.push_back([before, focus, lt](AppState& st2) {
                if (st2.window_tabs.Active() != lt) return;
                if (PaneAlive(st2, focus)) FocusPane(st2, focus);
                ApplyLayoutPreset(st2, before);
            });
        if (!run.has_first_layout) {
            run.first_layout = before;
            run.has_first_layout = true;
        }
    }
    st.ok = true;
    st.sub = PanesLine(s);
    run.layout = true;
    AddKeys(run, PresetName(m->p), m->key >= 0 ? KeysFor(app::KeyAction::Layout, m->key) : std::vector<std::wstring>());
    if (ShownPanes(s).size() > 1)
        AddKeys(run, Text(ShownPanes(s).size() == 2 ? I::AiCtlKeyNextPane2 : I::AiCtlKeyNextPane), KeysFor(app::KeyAction::NextPane));
    // panes: one open per named folder, right after this step.
    if (const json::Value* v = act.find("panes"); v && v->is_array()) {
        std::vector<json::Value> opens;
        for (size_t i = 0; i < v->size() && i < 4; ++i) {
            if (!v->at(i).is_string() || v->at(i).as_string().empty()) continue;
            json::Value args = json::Value::Object();
            args.set("path", v->at(i).as_string());
            args.set("pane", static_cast<uint64_t>(i + 1));
            json::Value o = json::Value::Object();
            o.set("action", "nav.go");
            o.set("args", std::move(args));
            opens.push_back(std::move(o));
        }
        if (run.open_targets.size() == run.queue.size())
            run.open_targets.insert(run.open_targets.begin() + static_cast<std::ptrdiff_t>(run.index + 1),
                                    opens.size(), CtlOpenTarget{});
        else
            run.open_targets.clear(); // An inconsistent queue must not inherit another step's authority.
        run.queue.insert(run.queue.begin() + static_cast<std::ptrdiff_t>(run.index + 1), opens.begin(), opens.end());
    }
    return st;
}

CtlStep CtlOpen(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Folder;
    const std::wstring path = CtlFolder(act.get_string("path"));
    std::vector<app::Pane*> picked;
    std::string pane_error;
    app::Pane* p = s.pane;
    if (const json::Value* v = act.find("pane"); v && !(v->is_string() && v->as_string() == "all")) {
        if (!app::actions::PanesForArg(s, v, picked, pane_error)) {
            st.title = Text(I::AiCtlStepOpen);
            st.arg = json::Wide(act.get_string("path"));
            st.error = pane_error;
            st.why = Text(I::AiActWhyFailed);
            return st;
        }
        p = picked.front();
    }
    const std::wstring pos = PanePos(s, PaneIndex(s, p));
    st.title = pos.empty() ? Text(I::AiCtlStepOpen) : Fill(Text(I::AiCtlStepOpenIn), L"{pane}", pos);
    if (path.empty()) {
        st.arg = json::Wide(act.get_string("path"));
        st.error = "no such folder";
        st.why = Text(I::AiActWhyNoFolder);
        return st;
    }
    st.arg = PlaceLabel(path);
    if (_wcsicmp(NormDir(path).c_str(), KnownFolder(FOLDERID_Desktop).c_str()) == 0) st.icon = ui::AiIcon::Desktop;
    app::Tab* tab = p ? p->ActiveTab() : nullptr;
    if (!tab) {
        st.error = "no pane";
        return st;
    }
    if (SameDir(tab->current_path, path)) {
        st.skip = true;
        st.ok = true;
        return st;
    }
    const std::wstring old = tab->current_path;
    WithPane(s, p, [&] { NavigateTo(s, path); });
    const std::wstring now = tab->current_path;
    run.undo.push_back([p, old, now](AppState& st2) {
        if (!PaneAlive(st2, p) || !p->ActiveTab() || p->ActiveTab()->current_path != now) return;
        WithPane(st2, p, [&] { NavigateTo(st2, old); });
    });
    st.ok = true;
    st.wait = p;
    run.open = true;
    return st;
}

CtlStep CtlNewWindow(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::NewWindow;
    st.title = Text(I::AiCtlStepNewWindow);
    std::wstring path;
    if (!act.get_string("path").empty()) {
        path = CtlFolder(act.get_string("path"));
        if (path.empty()) {
            st.arg = json::Wide(act.get_string("path"));
            st.error = "no such folder";
            st.why = Text(I::AiActWhyNoFolder);
            return st;
        }
    } else if (const app::Tab* tab = ActiveTab(s)) {
        path = tab->current_path;
    }
    st.arg = PlaceLabel(path);
    const HWND h = CtlOpenWindow(s, run, WindowInit{path});
    if (!h) {
        st.error = "the window could not be opened";
        return st;
    }
    AppCore* core = &s.core;
    run.undo.push_back([h, core](AppState& st) {
        if (!core->windows.Lookup(h)) return;
        ShowWindow(h, SW_HIDE);
        // Undo takes back a window the assistant opened: it is not one the
        // user closed, so no 已关闭窗口 entry and no 恢复 toast.
        const uint64_t seq = core->closed_windows.LatestSeq();
        DestroyWindow(h);
        if (core->closed_windows.LatestSeq() != seq) {
            core->closed_windows.TakeLatest();
            st.notification_toast.DismissFor(st.hwnd, WM_REOPEN_CLOSED_WINDOW);
        }
    });
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlNewWindowN), L"{n}", Num(s.core.windows.Count()));
    run.window = true;
    run.window_names.push_back(st.arg);
    run.back_parts.push_back(Fill(Text(I::AiCtlBackClosed), L"{name}", st.arg));
    AddKeys(run, Text(I::AiCtlKeyNewWindow), KeysFor(app::KeyAction::NewWindow));
    return st;
}

// The tab (tab_to_window) or one pane of the split (pane_to_window) moves
// to a window of its own; 撤销 puts it back where it was.
CtlStep CtlToWindow(AppState& s, CtlRun& run, const json::Value& act, bool pane_only) {
    CtlStep st;
    st.icon = ui::AiIcon::NewWindow;
    AppCore* core = &s.core;
    AppState* self = &s;
    auto panes = ShownPanes(s);
    if (!pane_only && s.window_tabs.items.size() < 2 && panes.size() >= 2) pane_only = true;   // the focused pane
    if (pane_only) {
        int64_t n = 0;
        app::Pane* p = s.pane;
        if (const json::Value* v = act.find("pane"); v && v->get_int(n, 1, 4) && static_cast<size_t>(n) <= panes.size())
            p = panes[static_cast<size_t>(n - 1)];
        const std::wstring pos = PanePos(s, PaneIndex(s, p));
        const std::wstring label = PlaceLabel(p && p->ActiveTab() ? p->ActiveTab()->current_path : std::wstring());
        st.title = Text(I::AiCtlStepTabToWindow);
        st.arg = label;
        if (panes.size() < 2 || !p) {
            st.error = "the window shows one pane; use tab_to_window or new_window";
            return st;
        }
        app::LayoutTab* into = s.window_tabs.Active();
        auto origin = std::make_shared<app::PaneOrigin>();
        const std::optional<size_t> tab_index = SplitPaneIntoTab(s, p, *origin);
        if (!tab_index) {
            st.error = "this pane cannot leave the split";
            return st;
        }
        WindowInit init;
        if (p->ActiveTab()) init.path = p->ActiveTab()->current_path;
        init.adopt_from = &s;
        init.adopt_index = *tab_index;
        const HWND h = CtlOpenWindow(s, run, init);
        // The split it left shows again here.
        for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
            if (s.window_tabs.items[i].get() == into) {
                if (s.window_tabs.active != i) SwitchTab(s, i);
                break;
            }
        if (!h) {
            st.error = "the window could not be opened";
            return st;
        }
        run.undo.push_back([h, core, self, into, origin](AppState&) {
            AppState* moved = core->windows.Lookup(h);
            if (!moved || moved->window_tabs.items.size() != 1) return;
            ShowWindow(h, SW_HIDE);
            if (ReturnTabAsPane(*moved, 0, *self, into, *origin, TransferSource::Closing)) DestroyWindow(h);
            else ShowWindow(h, SW_SHOWNORMAL);
        });
        st.ok = true;
        st.sub = Fill(Text(I::AiCtlNewWindowN), L"{n}", Num(s.core.windows.Count()));
        if (ShownPanes(s).size() == 1) run.single_back = true;
        run.window = true;
        run.window_names.push_back(label);
        run.back_parts.push_back(Fill(Fill(Text(I::AiCtlBackPane), L"{name}", label), L"{pane}", pos));
        AddKeys(run, Text(I::AiCtlKeyTabToWindow), KeysFor(app::KeyAction::TabToWindow));
        return st;
    }
    auto& tabs = s.window_tabs;
    const size_t index = tabs.active;
    const std::wstring label = PlaceLabel(ActiveTab(s) ? ActiveTab(s)->current_path : std::wstring());
    st.title = Text(I::AiCtlStepTabToWindow);
    st.arg = label;
    if (tabs.items.size() < 2 || index >= tabs.items.size()) {
        st.error = "the window has one tab; use new_window, or pane_to_window for a pane";
        return st;
    }
    WindowInit init;
    if (const app::Tab* view = ActiveTab(s)) init.path = view->current_path;
    init.adopt_from = &s;
    init.adopt_index = index;
    const size_t before = tabs.items.size();
    const HWND h = CtlOpenWindow(s, run, init);
    if (!h || s.window_tabs.items.size() == before) {
        st.error = "the window could not be opened";
        return st;
    }
    run.undo.push_back([h, core, self, index](AppState&) {
        AppState* moved = core->windows.Lookup(h);
        if (!moved || moved->window_tabs.items.size() != 1) return;
        ShowWindow(h, SW_HIDE);
        if (MoveLayoutTab(*moved, 0, *self, index, TransferSource::Closing)) DestroyWindow(h);
        else ShowWindow(h, SW_SHOWNORMAL);
    });
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlNewWindowN), L"{n}", Num(s.core.windows.Count()));
    run.window = true;
    run.window_names.push_back(label);
    run.back_parts.push_back(Fill(Text(I::AiCtlBackTab), L"{name}", label));
    AddKeys(run, Text(I::AiCtlKeyTabToWindow), KeysFor(app::KeyAction::TabToWindow));
    return st;
}

const wchar_t* ViewName(ui::ViewMode m) {
    static const I kNames[] = {I::AiCtlViewXl, I::AiCtlViewL, I::AiCtlViewM, I::AiCtlViewS,
                               I::AiCtlViewList, I::AiCtlViewDetails, I::AiCtlViewTiles, I::AiCtlViewContent};
    const int i = ui::ViewModeIndex(m);
    return l10n::Get(kNames[i >= 0 && i < 8 ? i : 5]).c_str();
}

// A result line from a sentence with `key` in it: the value goes in bold.
ui::AiResultLine SplitLine(const std::wstring& text, std::wstring_view key, const std::wstring& value) {
    ui::AiResultLine ln;
    const size_t at = key.empty() ? std::wstring::npos : text.find(key);
    if (at == std::wstring::npos) {
        ln.before = text;
        return ln;
    }
    ln.before = text.substr(0, at);
    ln.bold = value;
    ln.after = text.substr(at + key.size());
    return ln;
}

void AddLine(CtlRun& run, ui::AiResultLine ln) {
    run.single_title = ln.before + ln.bold + ln.after;
    run.lines.push_back(std::move(ln));
}

// One plain action (view, sort, bars, windows) done: its line.
void PlainDone(CtlRun& run, ui::AiResultLine ln) {
    run.other = true;
    AddLine(run, std::move(ln));
}

// The panes a view / sort / group step changes (the "pane" argument; "all"
// is every pane on screen). False: st has the error.
bool StepPanes(AppState& s, const json::Value& act, CtlStep& st, std::vector<app::Pane*>& out) {
    std::string error;
    if (app::actions::PanesForArg(s, act.find("pane"), out, error)) {
        std::erase_if(out, [](app::Pane* p) { return !p || !p->ActiveTab(); });
        if (!out.empty()) return true;
        error = "no tab";
    }
    st.error = error;
    st.why = Text(I::AiActWhyFailed);
    return false;
}

// "全部 2 栏" after the argument when the step changed more than one pane.
void AllPanesArg(CtlStep& st, size_t n) {
    if (n > 1) st.arg += L" \u00B7 " + Fill(Text(I::AiActAllPanes), L"{n}", Num(n));
}

CtlStep CtlView(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Rows;
    st.title = Text(I::AiCtlStepView);
    static const char* kIds[] = {"extra_large_icons", "large_icons", "medium_icons", "small_icons",
                                 "list", "details", "tiles", "content"};
    const std::string want = act.get_string("view", act.get_string("mode"));
    int idx = -1;
    for (int i = 0; i < 8; ++i)
        if (want == kIds[i]) idx = i;
    if (idx < 0) {
        st.error = "unknown view";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    std::vector<app::Pane*> panes;
    if (!StepPanes(s, act, st, panes)) return st;
    const ui::ViewMode mode = ui::ViewModeFromIndex(idx);
    st.arg = ViewName(mode);
    AllPanesArg(st, panes.size());
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlViewDone), L"{view}", ViewName(mode));
    AddKeys(run, ViewName(mode), KeysFor(app::KeyAction::ViewMode, idx));
    bool changed = false;
    for (app::Pane* p : panes) {
        const ui::ViewMode old = p->ActiveTab()->view_mode;
        if (old == mode) continue;
        WithPane(s, p, [&] { ChooseViewMode(s, mode); });
        run.undo.push_back([p, old](AppState& st2) {
            if (PaneAlive(st2, p)) WithPane(st2, p, [&] { ChooseViewMode(st2, old); });
        });
        changed = true;
    }
    if (!changed) {
        st.skip = true;
        return st;
    }
    PlainDone(run, SplitLine(Text(I::AiCtlLineView), L"{view}", ViewName(mode)));
    return st;
}

CtlStep CtlSort(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Sliders;
    st.title = Text(I::AiCtlStepSort);
    static const struct { const char* id; ui::SortColumn col; I name; } kCols[] = {
        {"name", ui::SortColumn::Name, I::AiCtlSortName},   {"modified", ui::SortColumn::Mtime, I::AiCtlSortModified},
        {"type", ui::SortColumn::Type, I::AiCtlSortType},   {"size", ui::SortColumn::Size, I::AiCtlSortSize},
        {"created", ui::SortColumn::Created, I::AiCtlSortCreated},
    };
    const std::string want = act.get_string("by", act.get_string("sort"));
    const auto* m = std::find_if(std::begin(kCols), std::end(kCols), [&](const auto& e) { return want == e.id; });
    if (m == std::end(kCols)) {
        st.error = "unknown sort";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    std::vector<app::Pane*> panes;
    if (!StepPanes(s, act, st, panes)) return st;
    const bool desc = act.get_bool("desc", m->col == ui::SortColumn::Mtime || m->col == ui::SortColumn::Size ||
                                               m->col == ui::SortColumn::Created);
    // The direction in the column's own words: 新的在前, 大的在前, A → Z.
    const bool dated = m->col == ui::SortColumn::Mtime || m->col == ui::SortColumn::Created;
    const std::wstring dir_text = Text(dated ? (desc ? I::AiActSortNew : I::AiActSortOld)
                                       : m->col == ui::SortColumn::Size ? (desc ? I::AiActSortBig : I::AiActSortSmall)
                                                                        : (desc ? I::AiActSortZA : I::AiActSortAZ));
    const std::wstring by = Text(m->name);
    st.arg = by + L" \u00B7 " + dir_text;
    AllPanesArg(st, panes.size());
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    const ui::SortDirection dir = desc ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    bool changed = false;
    for (app::Pane* p : panes) {
        const app::Tab* tab = p->ActiveTab();
        if (tab->sort_column == m->col && tab->sort_direction == dir) continue;
        const ui::SortColumn old_col = tab->sort_column;
        const ui::SortDirection old_dir = tab->sort_direction;
        WithPane(s, p, [&] { SetSort(s, m->col, dir); });
        run.undo.push_back([p, old_col, old_dir](AppState& st2) {
            if (PaneAlive(st2, p)) WithPane(st2, p, [&] { SetSort(st2, old_col, old_dir); });
        });
        changed = true;
    }
    if (!changed) {
        st.skip = true;
        return st;
    }
    const std::wstring which = panes.size() == 2 ? Text(I::AiActBothPanes)
                                                 : Fill(Text(I::AiActAllPanes), L"{n}", Num(panes.size()));
    PlainDone(run, SplitLine(Fill(Fill(Text(panes.size() > 1 ? I::AiActLineSortAll : I::AiActLineSort), L"{panes}", which),
                                  L"{dir}", dir_text),
                             L"{by}", by));
    return st;
}

CtlStep CtlGroup(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Rows;
    st.title = Text(I::AiCtlStepGroup);
    static const struct { const char* id; int v; I name; } kBy[] = {
        {"none", 0, I::AiCtlGroupNone}, {"name", 1, I::AiCtlSortName}, {"date", 2, I::AiCtlGroupDate},
        {"type", 3, I::AiCtlSortType},  {"size", 4, I::AiCtlSortSize},
    };
    const std::string want = act.get_string("by", act.get_string("group"));
    const auto* m = std::find_if(std::begin(kBy), std::end(kBy), [&](const auto& e) { return want == e.id; });
    if (m == std::end(kBy)) {
        st.error = "unknown group";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    std::vector<app::Pane*> panes;
    if (!StepPanes(s, act, st, panes)) return st;
    st.arg = Text(m->name);
    AllPanesArg(st, panes.size());
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    bool changed = false;
    for (app::Pane* p : panes) {
        const int old = p->ActiveTab()->group_by;
        if (old == m->v) continue;
        WithPane(s, p, [&] { SetGroupBy(s, m->v); });
        run.undo.push_back([p, old](AppState& st2) {
            if (PaneAlive(st2, p)) WithPane(st2, p, [&] { SetGroupBy(st2, old); });
        });
        changed = true;
    }
    if (!changed) {
        st.skip = true;
        return st;
    }
    PlainDone(run, m->v == 0 ? SplitLine(Text(I::AiCtlLineGroupOff), {}, {})
                             : SplitLine(Text(I::AiCtlLineGroup), L"{by}", Text(m->name)));
    return st;
}

// sidebar / favorites_bar: show (true) or hide.
CtlStep CtlBar(AppState& s, CtlRun& run, const json::Value& act, bool favorites) {
    CtlStep st;
    st.icon = favorites ? ui::AiIcon::Star : ui::AiIcon::Dock;
    st.title = Text(favorites ? I::AiCtlStepFavBar : I::AiCtlStepSidebar);
    const bool show = act.get_bool("show", true);
    st.arg = Text(favorites ? (show ? I::AiCtlShow : I::AiCtlHide) : (show ? I::AiCtlExpand : I::AiCtlCollapse));
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    AddKeys(run, Text(favorites ? I::AiCtlKeyFavBar : I::AiCtlKeySidebar),
            KeysFor(favorites ? app::KeyAction::FavoritesBar : app::KeyAction::Sidebar));
    const auto shown = [favorites](AppState& st2) {
        return favorites ? st2.appPrefs.favorites_bar : !st2.appPrefs.sidebar_collapsed;
    };
    if (shown(s) == show) {
        st.skip = true;
        return st;
    }
    if (favorites) ToggleFavoritesBar(s);
    else ToggleSidebarCollapsed(s);
    run.undo.push_back([favorites, show, shown](AppState& st2) {
        if (shown(st2) != show) return;
        if (favorites) ToggleFavoritesBar(st2);
        else ToggleSidebarCollapsed(st2);
    });
    PlainDone(run, SplitLine(Text(favorites ? (show ? I::AiCtlFavBarShown : I::AiCtlFavBarHidden)
                                            : (show ? I::AiCtlSidebarShown : I::AiCtlSidebarHidden)),
                             {}, {}));
    return st;
}

CtlStep CtlTray(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Tray;
    st.title = Text(I::AiCtlStepTray);
    const std::vector<std::wstring> paths = CtlItems(s, act);
    st.arg = Fill(Text(I::AiCtlItems), L"{n}", Num(paths.size()));
    if (paths.empty()) {
        st.error = "no items (select some, or pass full paths)";
        st.why = Text(I::AiActWhyNoItems);
        return st;
    }
    std::vector<uint64_t> before;
    for (const auto& b : s.tray.batches())
        for (const auto& it : b.items) before.push_back(it.id);
    StagePaths(s, paths, std::nullopt);
    std::vector<uint64_t> added;
    size_t total = 0;
    for (const auto& b : s.tray.batches())
        for (const auto& it : b.items) {
            ++total;
            if (std::find(before.begin(), before.end(), it.id) == before.end()) added.push_back(it.id);
        }
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlTrayNow), L"{n}", Num(total));
    if (!added.empty())
        run.undo.push_back([added](AppState& st2) { RemoveTrayItems(st2, added); });
    ++run.organize;
    run.organize_kinds.push_back(Text(I::AiCtlKindTray));
    AddLine(run, SplitLine(Text(I::AiCtlLineTray), L"{items}", st.arg));
    AddKeys(run, Text(I::AiCtlKeyTray), KeysFor(app::KeyAction::StageToTray));
    return st;
}

CtlStep CtlTag(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Tag;
    st.title = Text(I::AiCtlStepTag);
    std::wstring name = json::Wide(act.get_string("tag"));
    while (!name.empty() && std::iswspace(name.back())) name.pop_back();
    while (!name.empty() && std::iswspace(name.front())) name.erase(0, 1);
    const std::vector<std::wstring> paths = CtlItems(s, act);
    st.arg = name;
    if (name.empty() || name.size() > 40) {
        st.error = "a tag name is needed";
        return st;
    }
    if (paths.empty()) {
        st.error = "no items (select some, or pass full paths)";
        st.why = Text(I::AiActWhyNoItems);
        return st;
    }
    app::TagId id;
    for (const auto& t : s.places.tags)
        if (_wcsicmp(t.name.c_str(), name.c_str()) == 0) {
            id = t.id;
            name = t.name;
        }
    bool created = false;
    if (id.empty()) {
        const std::vector<uint32_t>& palette = TagColorPalette(s);
        id = s.places.CreateTag(name, palette.empty() ? 0xEF4444u : palette[s.places.tags.size() % palette.size()]);
        created = !id.empty();
        st.arg = Fill(Text(I::AiCtlTagNew), L"{name}", name);
    }
    const int index = s.places.FindTagIndex(id);
    if (index < 0) {
        st.error = "the tag could not be made";
        return st;
    }
    std::vector<std::wstring> fresh;
    for (const auto& p : paths)
        if (!s.places.PathHasTag(p, index)) fresh.push_back(p);
    if (!fresh.empty() && !ToggleTagForSelection(s, id, fresh)) {
        if (created) DeleteTagAndRefreshViews(s, id);
        st.error = "these items cannot take tags";
        return st;
    }
    run.undo.push_back([id, fresh, created](AppState& st2) {
        if (!st2.places.FindTag(id)) return;
        if (!fresh.empty() && st2.places.GetSelectionState(id, fresh) == app::TagSelectionState::All)
            ToggleTagForSelection(st2, id, fresh);
        if (created && st2.places.PathsForTag(id).empty()) DeleteTagAndRefreshViews(st2, id);
    });
    st.ok = true;
    const std::wstring on = Fill(Text(I::AiCtlTagOn), L"{n}", Num(paths.size()));
    st.sub = created ? Fill(Text(I::AiCtlTagNewDone), L"{name}", name) + L" \u00B7 " + on : on;
    // tag.create just made it: 新建标签报销，打在 1 项上 - one line, one thing.
    const bool joins = !created && run.arg_last_id == "tag.create" && run.arg_last_line + 1 == run.lines.size() &&
                       _wcsicmp(run.arg_new_tag.c_str(), name.c_str()) == 0;
    ui::AiResultLine line = SplitLine(Fill(Text(created || joins ? I::AiCtlLineTagNew : I::AiCtlLineTag), L"{n}", Num(paths.size())),
                                      L"{name}", name);
    if (joins) {
        run.single_title = line.before + line.bold + line.after;
        run.lines.back() = std::move(line);
        run.arg_last_id.clear();
        return st;
    }
    ++run.organize;
    run.organize_kinds.push_back(Text(I::AiCtlKindTag));
    AddLine(run, std::move(line));
    return st;
}

CtlStep CtlFavorite(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Star;
    st.title = Text(I::AiCtlStepFavorite);
    std::vector<std::wstring> paths;
    if (act.find("items")) paths = CtlItems(s, act);
    else if (const std::wstring one = json::Wide(act.get_string("path")); !one.empty()) {
        std::wstring p = ResolveTaskDest(one);
        if (p.empty()) p = one;
        if (Exists(p)) paths.push_back(NormDir(p));
    } else if (const app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        paths.push_back(tab->current_path);
    if (paths.empty()) {
        st.error = "nothing to add (pass full paths)";
        st.why = Text(I::AiActWhyNoItems);
        return st;
    }
    std::wstring names;
    for (size_t i = 0; i < paths.size() && i < 3; ++i) names += (names.empty() ? L"" : Text(I::AiTkListSep)) + PlaceLabel(paths[i]);
    if (paths.size() > 3) names += Fill(Text(I::AiCtlAndMore), L"{n}", Num(paths.size()));
    st.arg = names;
    std::vector<std::wstring> added;
    for (const auto& p : paths) {
        if (s.places.IsStarred(p)) continue;
        if (ToggleStarred(s, p, IsDirectory(p) ? app::PlaceItemKind::Folder : app::PlaceItemKind::File)) added.push_back(p);
    }
    if (!added.empty())
        run.undo.push_back([added](AppState& st2) {
            for (const auto& p : added)
                if (st2.places.IsStarred(p)) ToggleStarred(st2, p, app::PlaceItemKind::Unknown);
        });
    st.ok = true;
    st.sub = Text(I::AiCtlFavIn) + L" \u203A " + names;
    ++run.organize;
    run.organize_kinds.push_back(Text(I::AiCtlKindFav));
    AddLine(run, SplitLine(Text(I::AiCtlLineFav), L"{name}", names));
    return st;
}

CtlStep CtlMerge(AppState& s, CtlRun& run) {
    CtlStep st;
    st.icon = ui::AiIcon::Tiles;
    st.title = Text(I::AiCtlStepMerge);
    const size_t before = s.core.windows.Count();
    if (before < 2 || !MergeAllWindows(s)) {
        st.error = "there is only one window";
        st.why = Text(I::AiActWhyOneWindow);
        return st;
    }
    st.ok = true;
    st.sub = Fill(Text(I::AiCtlMerged), L"{n}", Num(before - s.core.windows.Count()));
    run.merged = true;
    PlainDone(run, SplitLine(st.sub, {}, {}));
    AddKeys(run, Text(I::AiCtlKeyMerge), KeysFor(app::KeyAction::MergeWindows));
    return st;
}

CtlStep CtlOverview(AppState& s, CtlRun& run) {
    CtlStep st;
    st.icon = ui::AiIcon::Tiles;
    st.title = Text(I::AiCtlStepOverview);
    if (!s.overview.open) ToggleWindowOverview(s);
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    PlainDone(run, SplitLine(Text(I::AiCtlLineOverview), {}, {}));
    AddKeys(run, Text(I::AiCtlStepOverview), KeysFor(app::KeyAction::Overview));
    return st;
}

// ---- P3: steps beyond the third batch's ----------------------------------

// The caps of a shown shortcut ("Ctrl+Shift+O" → Ctrl / Shift / O).
std::vector<std::wstring> CapsOf(const std::wstring& shortcut) {
    std::vector<std::wstring> caps;
    size_t at = 0;
    while (at < shortcut.size()) {
        size_t end = shortcut.find(L'+', at + 1);   // "Ctrl++" keeps its plus
        if (end == std::wstring::npos) end = shortcut.size();
        caps.push_back(shortcut.substr(at, end - at));
        at = end + 1;
    }
    return caps;
}

// No key for it: the card says what to type in the command palette, one
// row for the whole call (也可以自己输入 >详情面板 >文件夹大小).
void AddPaletteKey(CtlRun& run, const std::wstring& title) {
    const std::wstring cap = L">" + title;
    for (auto& k : run.keys)
        if (k.palette) {
            if (k.caps.size() < 3 && std::find(k.caps.begin(), k.caps.end(), cap) == k.caps.end()) k.caps.push_back(cap);
            return;
        }
    if (run.keys.size() >= 4) return;
    ui::AiKeyRowView row{Text(I::AiActPalette), {cap}};
    row.palette = true;
    run.keys.push_back(std::move(row));
}

// PDF / JPG: the extensions a selector names, for its step row.
std::wstring ExtsLabel(const app::actions::Selector& sel) {
    std::wstring out;
    for (size_t i = 0; i < sel.extensions.size(); ++i) {
        if (i == 3) { out += L"\u2026"; break; }
        std::wstring up = sel.extensions[i];
        for (wchar_t& c : up) c = static_cast<wchar_t>(std::towupper(c));
        out += (out.empty() ? L"" : L"/") + up;
    }
    return out;
}

// The step a selector's match makes: selects it (not part of the undo) and
// says so. In search results: 选中 搜索结果 · PDF · 41 项 · 12.6 MB, and the
// card 在搜索结果里选中了 41 项 / 分布在 9 个文件夹 (P4 scene 5).
CtlStep SelectStep(AppState& s, CtlRun& run, const app::actions::Selector& sel,
                   const app::actions::SelectorMatch& m) {
    CtlStep st;
    st.icon = ui::AiIcon::CheckCircle;
    st.title = Text(I::AiActStepSelect);
    const size_t index = m.pane ? PaneIndex(s, m.pane) : 0;
    const std::wstring pos = m.pane ? PanePos(s, index) : std::wstring();
    const app::Tab* tab = m.pane ? m.pane->ActiveTab() : nullptr;
    st.arg = m.results ? Text(I::AiActArgResults)
             : !pos.empty() ? pos
                            : PlaceLabel(tab ? tab->current_path : std::wstring());
    st.data = app::actions::ApplySelection(s, m);
    if (m.results && run.search_partial) st.data.set("search_complete", false);
    if (!m.reason.empty()) {
        st.error = m.reason == "tag_in_results"
                       ? "tag_in_results: tags are not matched in content search results; search with the tag instead"
                   : m.reason == "changed" ? "changed: the search results changed while matching; try again"
                                           : m.reason;
        st.why = Text(m.reason == "loading" ? I::AiActWhyLoading
                      : m.reason == "no_match" || m.reason == "filter_hides" || m.reason == "unknown_tag"
                          ? I::AiActWhyNoMatch
                          : I::AiActWhyFailed);
        return st;
    }
    st.ok = true;
    const std::wstring items = Fill(Text(I::AiCtlItems), L"{n}", Num(m.matched.size()));
    st.sub = m.bytes > 0 ? items + L" \u00B7 " + AiChatFormatSize(m.bytes) : items;
    run.other = true;
    if (m.results) {
        const std::wstring exts = ExtsLabel(sel);
        if (!exts.empty()) st.sub = exts + L" \u00B7 " + st.sub;
        run.title_override = Fill(Text(I::AiActResSelectResults), L"{items}", items);
        run.sub_override = Fill(Text(I::AiActResFolders), L"{n}", Num(m.folders));
        if (exts.empty()) {
            // Nothing the title does not say already.
            AddLine(run, SplitLine(run.title_override, items, items));
            run.override_alone = true;
        } else {
            // 选中了 41 个 PDF under it.
            const std::wstring what = Fill(Fill(Text(I::AiActCountExts), L"{n}", Num(m.matched.size())), L"{exts}", exts);
            AddLine(run, SplitLine(Fill(Text(I::AiActLineSelect), L"{items}", what), what, what));
        }
        return st;
    }
    AddLine(run, pos.empty() ? SplitLine(Fill(Text(I::AiActLineSelect), L"{items}", items), items, items)
                             : SplitLine(Fill(Fill(Text(I::AiActLineSelectIn), L"{pane}", pos), L"{items}", items),
                                         items, items));
    return st;
}

bool ParseStepSelector(const json::Value& act, app::actions::Selector& sel, std::string& error) {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return app::actions::ParseSelector(act, app::actions::FileTimeTicks(ft), sel, error);
}

// select.items: P2's selector on one pane, like the user's own selection
// (not part of the undo). Waits for the listing first (CtlNext); a search
// page for all its results (P4d).
CtlStep CtlSelect(AppState& s, CtlRun& run, const json::Value& act) {
    app::actions::Selector sel;
    std::string error;
    if (!ParseStepSelector(act, sel, error)) {
        CtlStep st;
        st.icon = ui::AiIcon::CheckCircle;
        st.title = Text(I::AiActStepSelect);
        st.error = error;
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    return SelectStep(s, run, sel, app::actions::ResolveSelector(s, sel));
}

// select.items on a content search's results: the store matches them on its
// own thread (P4d); run->file_step then holds the step. False: not such a
// pane (or a bad selector), CtlSelect does it.
bool SelectAsk(AppState& s, std::shared_ptr<CtlRun> run, const json::Value& act) {
    app::actions::Selector sel;
    std::string error;
    if (!ParseStepSelector(act, sel, error)) return false;
    const bool started = app::actions::ResolveSelectorAsync(
        s, sel, [run, sel](AppState& s2, app::actions::SelectorMatch m) {
            if (RunGone(s2, *run)) return;
            run->file_step = std::make_shared<CtlStep>(SelectStep(s2, *run, sel, m));
            run->file_state = 2;
            CtlNext(s2, run);
        });
    if (started) run->file_state = 1;
    return started;
}

CtlStep CtlSelectNone(AppState& s, CtlRun& run) {
    CtlStep st;
    st.icon = ui::AiIcon::CheckCircle;
    st.title = Text(I::ActSelectNone);
    app::Tab* tab = ActiveTab(s);
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    if (!tab || tab->SelectedCount() == 0) {
        st.skip = true;
        return st;
    }
    tab->ClearSelection();
    run.other = true;
    AddLine(run, SplitLine(Text(I::AiActLineClear), {}, {}));
    return st;
}

// The file nav.open opens: a full path, a name in the open folder, or the
// one selected item.
std::wstring OpenTarget(AppState& s, const json::Value& act) {
    std::wstring p = json::Wide(act.get_string("path"));
    const app::Tab* tab = ActiveTab(s);
    if (p.empty()) {
        if (tab && tab->snapshot && tab->SelectedCount() == 1) {
            const auto sel = SelectedFullPaths(*tab);
            if (!sel.empty()) return sel.front();
        }
        return {};
    }
    if (const std::wstring known = ResolveTaskDest(p); !known.empty()) return known;
    if (Exists(p)) return p;
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
        p.find_first_of(L"\\/:") == std::wstring::npos) {
        const std::wstring in = NormDir(tab->current_path) + L"\\" + p;
        if (Exists(in)) return in;
    }
    return {};
}

const std::wstring& BoundOpenTarget(const CtlRun& run, size_t index) {
    static const std::wstring empty;
    return run.open_targets.size() == run.queue.size() && index < run.open_targets.size()
               ? run.open_targets[index].path : empty;
}

std::string BindOpenTargets(AppState& s, CtlRun& run) {
    run.open_targets.assign(run.queue.size(), {});
    for (size_t i = 0; i < run.queue.size(); ++i) {
        if (StepAction(run.queue[i]) != "nav.open") continue;
        const json::Value& args = StepArgs(run.queue[i]);
        const std::wstring supplied = json::Wide(args.get_string("path"));
        const bool absolute = (supplied.size() >= 3 && supplied[1] == L':' &&
                               (supplied[2] == L'\\' || supplied[2] == L'/')) ||
                              (supplied.size() >= 2 && supplied[0] == L'\\' && supplied[1] == L'\\');
        auto fail = [&](const char* reason) {
            run.open_targets.clear();
            return "step " + std::to_string(i + 1) + " (nav.open): " + reason + "; nothing of this call was done";
        };
        // A later selected item/name could mean the result of an earlier step.
        // Binding today's selection would silently open the wrong object.
        if (i > 0 && !absolute)
            return fail("a later open must use a full path; split selection/navigation and open into separate calls");
        const std::wstring target = OpenTarget(s, args);
        if (target.empty()) return fail("no target could be resolved; pass a full path or select one item first");
        const DWORD needed = GetFullPathNameW(target.c_str(), 0, nullptr, nullptr);
        if (!needed) return fail("the open target could not be made absolute");
        std::wstring full(needed, L'\0');
        const DWORD written = GetFullPathNameW(target.c_str(), needed, full.data(), nullptr);
        if (!written || written >= needed) return fail("the open target could not be made absolute");
        full.resize(written);
        run.open_targets[i].path = std::move(full);
    }
    return {};
}

std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

CtlStep CtlOpenFile(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Open;
    st.title = Text(I::AiCtlStepOpen);
    const std::wstring path = BoundOpenTarget(run, run.index);
    if (path.empty()) {
        st.arg = json::Wide(act.get_string("path"));
        st.error = "open target was not bound before authorization; start a new run_actions call";
        st.why = Text(I::AiActWhyNoFile);
        return st;
    }
    st.arg = LeafName(path);
    const app::actions::McpGroup group = app::actions::McpGroupOf("nav.open", path);
    if (run.mcp && !run.mcp->allowed[static_cast<size_t>(group)]) {
        st.error = app::ai::McpNotAllowed(group, "nav.open").error.message;
        st.why = Text(I::AiActWhyLocked);
        return st;
    }
    if (app::actions::AiPolicy("nav.open", path).gate == app::actions::Gate::Confirm &&
        !run.open_targets[run.index].confirmed) {
        st.error = "this open target requires its own confirmation before execution";
        st.why = Text(I::AiActWhyDeclined);
        return st;
    }
    if (IsDirectory(path)) {
        json::Value go = json::Value::Object();
        go.set("path", Utf8(path));
        return CtlOpen(s, run, go);
    }
    OpenPath(s, path);
    st.ok = true;
    st.sub = Text(I::AiCtlApplied);
    run.other = run.touched = true;
    AddLine(run, SplitLine(Fill(Text(I::AiActLineOpened), L"{name}", st.arg), st.arg, st.arg));
    return st;
}

// file.new_folder with a name: made in the open folder at once (a free name:
// 报销 (2) when 报销 exists); 撤销 removes it while it is still empty.
CtlStep CtlNewFolder(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::FolderPlus;
    st.title = Text(I::NewFolder);
    std::wstring name = json::Wide(act.get_string("name"));
    while (!name.empty() && (std::iswspace(name.back()) || name.back() == L'.')) name.pop_back();
    while (!name.empty() && std::iswspace(name.front())) name.erase(0, 1);
    st.arg = name;
    const app::Tab* tab = ActiveTab(s);
    if (name.empty() || name.size() > 120 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        st.error = "a folder name is needed (no \\ / : * ? \" < > |)";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) {
        st.error = "the active tab shows no folder";
        st.why = Text(I::ActWhyNotFolder);
        return st;
    }
    const std::wstring base = NormDir(tab->current_path) + L"\\";
    std::wstring made = name;
    for (int n = 2; Exists(base + made) && n < 100; ++n) made = name + L" (" + std::to_wstring(n) + L")";
    const std::wstring full = base + made;
    if (Exists(full) || !CreateDirectoryW(full.c_str(), nullptr)) {
        st.error = "the folder could not be made";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    run.undo.push_back([full](AppState&) { RemoveDirectoryW(full.c_str()); });   // fails unless empty
    run.touched = true;
    st.ok = true;
    st.arg = made;
    st.sub = Text(I::AiCtlApplied);
    st.data = json::Value::Object();
    st.data.set("path", Utf8(full));
    run.other = true;
    AddLine(run, SplitLine(Fill(Text(I::AiActLineNewFolder), L"{name}", made), made, made));
    AddKeys(run, Text(I::NewFolder), KeysFor(app::KeyAction::NewFolder));
    return st;
}

// A setting's value from the model: a switch takes true / false, a choice
// its key (numbers such as 7 may come as numbers). -1: invalid.
int SettingIndex(const app::actions::SetDef& d, const json::Value* val) {
    int index = -1;
    if (app::actions::IsSwitch(d)) {
        if (val && val->is_bool()) index = val->as_bool() ? 1 : 0;
        else if (val && val->is_string()) {
            const std::string& t = val->as_string();
            index = t == "true" || t == "on" ? 1 : t == "false" || t == "off" ? 0 : -1;
        }
    } else if (val) {
        std::string key = val->is_string() ? val->as_string()
                          : val->is_number() ? std::to_string(static_cast<int>(val->as_number())) : std::string();
        if (key == "mica-alt") key = "mica_alt";
        for (size_t k = 0; k < d.choices.size(); ++k)
            if (d.choices[k].key && key == d.choices[k].key) index = static_cast<int>(k);
    }
    return index;
}

bool LockedSettingKey(std::string_view id) {
    return id == "assistant_permissions" || id == "always_allow" || id == "connections" || id == "api_key";
}

// settings.set among other steps: one line old → new; a call of only
// settings gets the settings card instead (ToolChangeSettings).
CtlStep CtlSetting(AppState& s, CtlRun& run, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Gear;
    st.title = Text(I::AiSetStep);
    const std::string id = act.get_string("key", act.get_string("id"));
    st.arg = json::Wide(id);
    if (LockedSettingKey(id)) {
        run.locked = true;
        st.error = "locked: only the user can change this in Pulse's settings; Pulse showed them why";
        st.why = Text(I::AiActWhyLocked);
        return st;
    }
    const app::actions::SetDef* d = app::actions::FindSetting(id);
    if (!d) {
        st.error = "unknown setting";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    st.arg = Text(d->label);
    const int index = SettingIndex(*d, act.find("value"));
    if (index < 0) {
        st.error = "invalid value";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    if (id == "confirm_delete" && index == 0) {
        run.locked = run.deny_delete = true;
        st.error = "locked: the delete confirmation can never be turned off by the assistant; Pulse showed the user why";
        st.why = Text(I::AiActWhyLocked);
        return st;
    }
    const int from = d->get(s);
    const int raw_theme = s.appPrefs.theme_mode;
    const int want = app::actions::IsSwitch(*d) ? index : d->choices[static_cast<size_t>(index)].value;
    st.ok = true;
    if (from == want) {
        st.skip = true;
        st.sub = app::actions::SettingText(*d, from);
        return st;
    }
    app::actions::ApplySetting(s, *d, index);
    const int to = d->get(s);
    if (to != want) {
        if (to != from) app::actions::ApplySetting(s, *d, app::actions::ChoiceIndex(*d, from));
        st.ok = false;
        st.error = "Pulse could not change it";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const std::wstring old_text = app::actions::SettingText(*d, from), new_text = app::actions::SettingText(*d, to);
    st.sub = old_text + L" \u2192 " + new_text;
    if (d->flags & app::actions::kSetSys) st.sub += L" \u00B7 " + Text(I::AiSetSys);
    st.data = json::Value::Object();
    st.data.set("from", Utf8(old_text));
    st.data.set("to", Utf8(new_text));
    if (d->flags & app::actions::kSetSys) st.data.set("affects_windows", true);
    run.undo.push_back([d, from, to, raw_theme](AppState& st2) {
        if (d->get(st2) != to) return;
        const int idx = app::actions::ChoiceIndex(*d, from);
        if (idx >= 0) app::actions::ApplySetting(st2, *d, idx);
        if (std::string_view(d->id) == "theme" && raw_theme < 0) {
            st2.appPrefs.theme_mode = raw_theme;   // the legacy session theme stays legacy
            st2.appPrefs.Save();
        }
    });
    run.other = true;
    const std::wstring label = Text(d->label);
    AddLine(run, SplitLine(Fill(Fill(Fill(Text(I::AiActLineValue), L"{title}", label), L"{from}", old_text), L"{to}",
                                new_text),
                           new_text, new_text));
    return st;
}

// Any other row of the action table: what its menu row or palette row does.
// "choice" picks one of its choices (title or position); "show" (or "on")
// sets an on / off row. A switch or a choice undoes; the rest do not.
CtlStep CtlGeneric(AppState& s, CtlRun& run, const std::string& id, const json::Value& act) {
    CtlStep st;
    st.icon = ui::AiIcon::Bolt;
    const auto views = app::actions::ListActions(s);
    const auto find = [&](const std::vector<app::actions::ActionView>& list) -> const app::actions::ActionView* {
        for (const auto& v : list)
            if (v.id == id && v.key.empty()) return &v;
        return nullptr;
    };
    const app::actions::ActionView* v = find(views);
    if (!v) {
        st.title = Display(id);
        st.error = "unknown action (find_actions lists them)";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    st.title = v->title;
    if (!v->enabled) {
        st.error = "not available now: " + Utf8(v->why);
        st.why = v->why;
        return st;
    }
    int choice = -1;
    if (const json::Value* c = act.find("choice")) {
        int64_t n = 0;
        if (c->get_int(n, 0, static_cast<int64_t>(v->choices.size()) - 1)) choice = static_cast<int>(n);
        else if (c->is_string()) {
            const std::wstring want = json::Wide(c->as_string());
            for (size_t i = 0; i < v->choices.size() && choice < 0; ++i)
                if (_wcsicmp(v->choices[i].text.c_str(), want.c_str()) == 0) choice = static_cast<int>(i);
            for (size_t i = 0; i < v->choices.size() && choice < 0; ++i)
                if (!want.empty() && v->choices[i].text.find(want) != std::wstring::npos) choice = static_cast<int>(i);
        }
        if (choice < 0) {
            st.error = "no such choice (find_actions lists them)";
            st.why = Text(I::AiActWhyFailed);
            return st;
        }
        st.arg = v->choices[static_cast<size_t>(choice)].text;
    } else if (!v->choices.empty()) {
        std::string labels;
        for (const auto& c2 : v->choices) labels += (labels.empty() ? "" : " | ") + Utf8(c2.text);
        st.error = "choose one with \"choice\": " + labels;
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const std::wstring on = Text(I::AiSetOn), off = Text(I::AiSetOff);
    const bool is_switch = v->value == on || v->value == off;
    const json::Value* show = act.find("show");
    if (!show) show = act.find("on");
    if (is_switch && show && show->is_bool() && (v->value == on) == show->as_bool()) {
        st.ok = true;
        st.skip = true;
        st.sub = v->value;
        return st;
    }
    const std::wstring before = v->value, title = v->title, shortcut = v->shortcut;
    const int index = v->index;
    const auto old_choice = [&]() -> int {
        for (size_t i = 0; i < v->choices.size(); ++i)
            if (v->choices[i].radio) return static_cast<int>(i);
        return -1;
    }();
    if (!app::actions::RunAction(s, index, choice)) {
        st.error = "Pulse could not run it now";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    const auto after_views = app::actions::ListActions(s);
    const app::actions::ActionView* now = find(after_views);
    const std::wstring after = now ? now->value : std::wstring();
    st.ok = true;
    if (!before.empty() && !after.empty() && before != after) {
        st.sub = before + L" \u2192 " + after;
        AddLine(run, SplitLine(Fill(Fill(Fill(Text(I::AiActLineValue), L"{title}", title), L"{from}", before), L"{to}", after),
                               after, after));
        st.data = json::Value::Object();
        st.data.set("from", Utf8(before));
        st.data.set("to", Utf8(after));
        if (is_switch)
            run.undo.push_back([index, id, after](AppState& st2) {
                for (const auto& w : app::actions::ListActions(st2))
                    if (w.index == index && w.id == id && w.value == after) app::actions::RunAction(st2, index);
            });
        else if (old_choice >= 0)
            run.undo.push_back([index, id, after, old_choice](AppState& st2) {
                for (const auto& w : app::actions::ListActions(st2))
                    if (w.index == index && w.id == id && w.value == after) app::actions::RunAction(st2, index, old_choice);
            });
    } else {
        st.sub = Text(I::AiCtlApplied);
        AddLine(run, SplitLine(st.arg.empty() ? title : title + L" \u00B7 " + st.arg, {}, {}));
    }
    run.other = true;
    if (!shortcut.empty()) AddKeys(run, title, CapsOf(shortcut));
    else AddPaletteKey(run, title);
    return st;
}

void CtlFinish(AppState& s, std::shared_ptr<CtlRun> run) {
    if (run->mcp) {
        McpFinish(s, run);
        return;
    }
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const size_t undoable = run->undo.size();
    if (AiBlockView* b = Block(s, a.steps_id)) {
        b->label = Fill(Text(I::AiCtlStepsLabel), L"{n}", Num(run->ok));
        if (run->window && !run->layout && !run->other && !run->organize)
            b->label = Fill(Text(I::AiCtlStepsWindows), L"{n}", Num(run->window_names.size()));
        else if ((run->layout || run->open) && !run->window && !run->other && !run->organize)
            b->label = Fill(Text(I::AiCtlStepsLayout), L"{n}", Num(run->ok));
        if (run->ok == 0) {
            b->label = Text(I::AiCtlStepsNone);   // not 做了 0 件事
            b->partial = true;                    // a muted mark, not the green check
        }
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
    const bool failed = !run->fail_title.empty();
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", run->ok > 0 && !failed);
    out.data.set("done", static_cast<uint64_t>(run->ok));
    if (failed) out.data.set("stopped", "at the step that failed; later steps were not run, earlier ones stay done");
    out.data.set("steps", std::move(run->report));
    if (run->locked) {
        // A locked setting: why, and where the user changes it (third batch).
        AiBlockView& e = c.Add(AiBlockKind::Error, Now());
        e.tone = ui::AiErrTone::Warn;
        e.icon = ui::AiIcon::Lock;
        if (!run->lock_title.empty()) {
            // 这一项只能你自己操作 / why / 此电脑 → 双击 E 盘 (P4 scene 6).
            e.title = run->lock_title;
            e.sub = run->lock_sub;
            if (!run->lock_where.empty()) e.pills = {run->lock_where};
        } else {
            e.title = Text(run->deny_delete ? I::AiSetDenyT : I::AiSetLockT);
            e.sub = Text(run->deny_delete ? I::AiSetDenyS : I::AiSetLockS);
            e.hint = Text(I::AiSetLockHint);
            e.hint_icon = ui::AiIcon::Shield;
            e.pills = {Text(I::AiSetPillPerm), Text(I::AiSetPillAllow), Text(I::AiSetPillConn)};
        }
    }
    // Only a call that changed something gets a card (all "already so": the
    // model hears it, nothing to show or undo).
    const bool changed = !run->undo.empty() || run->merged || run->window || !run->lines.empty();
    if (run->ok > 0 && changed && !(failed && run->locked && run->lines.empty())) {
        AiBlockView& r = c.Add(AiBlockKind::Result, Now());
        const size_t things = run->organize + (run->other ? 1 : 0) + (run->window ? 1 : 0) + ((run->layout || run->open) ? 1 : 0);
        // The title: what the turn was about.
        if (failed) {
            // 做好了 1 件，1 件没做成: what was done, the step that failed, what was not done.
            r.partial = true;
            r.title = Fill(Fill(Text(I::AiActPartial), L"{ok}", Num(run->ok)), L"{bad}", Num(1));
            r.sub = Text(I::AiActPartialSub);
            r.lines = run->lines;
            ui::AiResultLine bad = SplitLine(Fill(Fill(Fill(Text(I::AiActFailLine), L"{title}", run->fail_title), L"{why}",
                                                       run->fail_why),
                                                  L"{arg}", run->fail_arg),
                                             run->fail_arg, run->fail_arg);
            bad.warn = true;
            r.lines.push_back(std::move(bad));
            for (const auto& t : run->not_run) {
                ui::AiResultLine ln = SplitLine(Fill(Text(I::AiActNotRun), L"{title}", t), {}, {});
                ln.mute = true;
                r.lines.push_back(std::move(ln));
            }
        } else if (run->window && !run->other && !run->organize) {
            r.title = Fill(Text(I::AiCtlResWindows), L"{n}", Num(run->window_names.size()));
            for (const auto& n : run->window_names) r.sub += (r.sub.empty() ? L"" : L" \u00B7 ") + n;
            if (run->single_back) r.sub += Text(I::AiCtlResMainSingle);
        } else if ((run->layout || run->open) && !run->other && !run->organize) {
            const app::LayoutPreset p = LayoutOf(s);
            const I t = p == app::LayoutPreset::Single          ? I::AiCtlResSingle
                        : p == app::LayoutPreset::TwoVertical   ? I::AiCtlResTwoV
                        : p == app::LayoutPreset::TwoHorizontal ? I::AiCtlResTwoH
                        : p == app::LayoutPreset::Three         ? I::AiCtlResThree
                        : p == app::LayoutPreset::FourGrid      ? I::AiCtlResFour
                                                                : I::AiCtlResLayout;
            r.title = run->layout ? Text(t) : Text(I::AiCtlResOpened);
            r.sub = PanesLine(s);
        } else if (things == 1 && run->lines.size() == 1 && !run->title_override.empty()) {
            r.title = run->title_override;
            r.sub = run->sub_override;
            if (!run->override_alone) r.lines = run->lines;
        } else if (things == 1 && run->lines.size() == 1) {
            r.title = run->single_title;
            if (run->organize) r.sub = Text(I::AiCtlResOrganizeSub);
        } else {
            r.lines = run->lines;
            if (run->layout) {
                // 布局 单栏 → 左右两栏：下载 | 文档, first: the rest happened in it.
                const app::LayoutPreset now_layout = LayoutOf(s);
                std::wstring folders;
                for (app::Pane* p : ShownPanes(s)) {
                    const app::Tab* t = p->ActiveTab();
                    folders += (folders.empty() ? L"" : L" | ") + PlaceLabel(t ? t->current_path : std::wstring());
                }
                const std::wstring to = PresetName(now_layout);
                const std::wstring from = run->has_first_layout && run->first_layout != now_layout
                                              ? std::wstring(PresetName(run->first_layout))
                                              : std::wstring();
                std::wstring text = Fill(Fill(Text(from.empty() ? I::AiActLineLayoutTo : I::AiActLineLayout), L"{from}", from),
                                         L"{panes}", folders);
                r.lines.insert(r.lines.begin(), SplitLine(Fill(text, L"{to}", to), to, to));
            }
            r.title = Fill(Text(I::AiCtlResThings), L"{n}",
                           Num(r.lines.size() + (run->window ? 1 : 0) + ((run->open && !run->layout) ? 1 : 0)));
            if (run->organize && !run->other && !run->window && !run->layout && !run->open) r.sub = Text(I::AiCtlResOrganizeSub);
            else if (!run->touched) r.sub = Text(I::AiActResSub);
        }
        // Keys only where they help: layout, windows, view; the organize
        // lines say enough.
        if (!failed && (run->layout || run->window || run->other)) r.keys = run->keys;
        if (!failed && run->window && run->keys.size() < 4) {
            ui::AiKeyRowView merge{Text(I::AiCtlKeyMerge), KeysFor(app::KeyAction::MergeWindows)};
            bool have = false;
            for (const auto& k : r.keys) have |= k.label == merge.label;
            if (!have && !merge.caps.empty()) r.keys.push_back(std::move(merge));
        }
        if (undoable) r.deny = Text(undoable > 1 && run->lines.size() > 1 ? I::AiCtlUndoAll : I::AiAsstUndo);
        if (run->window) {
            r.accept = Text(I::AiCtlOverview);
            r.accept_icon = ui::AiIcon::Tiles;
        } else if (run->settings_page >= 0 && !failed) {
            r.accept = Text(I::AiSetShow);
            r.accept_icon = ui::AiIcon::Gear;
        }
        c.result_id = r.id;
        const int page = run->settings_page;
        c.on_show = run->window ? std::function<void(AppState&)>([](AppState& st) {
            if (!st.overview.open) ToggleWindowOverview(st);
        })
                    : page >= 0 && !failed ? std::function<void(AppState&)>([page](AppState& st) { OpenSettingsTab(st, page); })
                                           : std::function<void(AppState&)>();
        if (undoable) {
            c.on_undo = [run](AppState& st) {
                HideRenameOverlay(st, true);   // a rename field must not stay over a moved pane
                for (auto it = run->undo.rbegin(); it != run->undo.rend(); ++it) (*it)(st);
                AiChat& c2 = Chat(st);
                c2.peek_undo = false;
                AiChatHidePeek(st);
                if (AiBlockView* rr = c2.Find(c2.result_id)) {
                    rr->done = true;
                    std::wstring t;
                    if (run->window) {
                        for (const auto& p : run->back_parts) t += (t.empty() ? L"" : Text(I::AiCtlBackSep)) + p;
                    } else if ((run->layout || run->open) && !run->other && !run->organize) {
                        t = run->has_first_layout ? Fill(Text(I::AiCtlUndoneLayout), L"{layout}", PresetName(run->first_layout))
                                                  : Text(I::AiCtlUndoneOpen);
                    } else if (run->organize && !run->other) {
                        std::wstring kinds;
                        for (size_t i = 0; i < run->organize_kinds.size(); ++i) {
                            if (std::find(run->organize_kinds.begin(), run->organize_kinds.begin() + static_cast<std::ptrdiff_t>(i),
                                          run->organize_kinds[i]) != run->organize_kinds.begin() + static_cast<std::ptrdiff_t>(i))
                                continue;
                            kinds += (kinds.empty() ? L"" : Text(I::AiTkListSep)) + run->organize_kinds[i];
                        }
                        t = Fill(Text(run->organize_kinds.size() > 1 ? I::AiCtlUndoneKinds : I::AiCtlUndoneKind), L"{kinds}", kinds);
                    } else {
                        t = Text(I::AiCtlUndoneAll);
                    }
                    rr->title = Text(I::AiAsstUndone);
                    rr->sub = t;   // the panel hides lines / keys / warn of a done card
                }
                AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
                note.text = run->window && !run->other && !run->organize ? Text(I::AiCtlNoteWindows)
                            : (run->layout || run->open) && !run->other && !run->organize && !run->window
                                ? Text(I::AiCtlNoteLayout)
                            : run->undo_steps <= 1 ? Text(I::AiActNoteOne)
                                                   : Fill(Text(I::AiCtlNoteThings), L"{n}", Num(run->undo_steps));
                AiChatSetChip(c2, ui::StatusAiState::Done, Text(I::AiAsstUndone));
                if (c2.agent)
                    c2.agent->pending_note =
                        L"(The user clicked Undo in Pulse: the changes of the last run_actions call were reversed; a selection stays as it was.)";
                Redraw(st);
            };
        } else {
            c.on_undo = nullptr;
        }
        c.peek_text = r.title;
        c.peek_undo = undoable > 0;
        AiChatSetChip(c, ui::StatusAiState::Busy, r.title, 0.95f);
    }
    AddToolResult(a, run->call_id, out);
    ++a.call_index;
    NextTool(s);
}

// The step's action id; control_pulse's "do" names still work.
std::string StepAction(const json::Value& step) {
    std::string id = step.get_string("action");
    if (!id.empty()) return id;
    static const std::pair<const char*, const char*> kOld[] = {
        {"layout", "window.layout"},      {"open", "nav.go"},           {"new_window", "window.new"},
        {"tab_to_window", "tab.to_window"}, {"pane_to_window", "pane.to_window"}, {"merge_windows", "window.merge"},
        {"window_overview", "window.overview"}, {"view", "view.mode"},  {"sort", "view.sort"},
        {"group", "view.group"},          {"sidebar", "sidebar.toggle"}, {"favorites_bar", "favbar.toggle"},
        {"tray", "tray.add"},             {"tag", "tag.toggle"},        {"favorite", "fav.add"},
    };
    const std::string old = step.get_string("do");
    for (const auto& [from, to] : kOld)
        if (old == from) return to;
    return old;
}

// Its arguments: "args", or (control_pulse) the step itself.
const json::Value& StepArgs(const json::Value& step) {
    const json::Value* args = step.find("args");
    return args && args->is_object() ? *args : step;
}

// The title a step shows when it was not done (排序：没有做).
std::wstring StepTitle(AppState& s, const std::string& id) {
    if (id == "settings.set") return Text(I::AiSetStep);
    if (id == "nav.go") return Text(I::AiCtlStepOpen);
    for (const auto& v : app::actions::ListActions(s))
        if (v.id == id && v.key.empty()) return v.title;
    return Display(id);
}

// P4: an action with a runner (action_args.h) - the tab, tag, name or keys
// the model gave. The runner says what changed in the card's words; here
// it becomes the step, the result line and the undo.
ui::AiIcon ArgIcon(std::string_view id) {
    struct Pair {
        std::string_view id;
        ui::AiIcon icon;
    };
    static constexpr Pair kExact[] = {
        {"tab.pin", ui::AiIcon::Pin},          {"tab.color", ui::AiIcon::Palette},
        {"tab.rename", ui::AiIcon::Rename},    {"tab.group", ui::AiIcon::Tiles},
        {"tab.duplicate", ui::AiIcon::Duplicate}, {"tab.close_others", ui::AiIcon::Close},
        {"tab.activate", ui::AiIcon::Window},  {"pane.focus", ui::AiIcon::Split},
        {"window.activate", ui::AiIcon::NewWindow}, {"sidebar.section", ui::AiIcon::Rows},
        {"view.icon_size", ui::AiIcon::Size},  {"smartview.save", ui::AiIcon::Star},
        {"recent.remove", ui::AiIcon::Clock},  {"alias.set", ui::AiIcon::Rename},
        {"index.status", ui::AiIcon::Search},  {"net.place", ui::AiIcon::Plug},
        {"git.push", ui::AiIcon::Push},        {"git.pull", ui::AiIcon::Pull},
        {"shell.verbs", ui::AiIcon::Package},  {"shell.run_verb", ui::AiIcon::Package},
        {"toolbar.open_with", ui::AiIcon::Open}, {"drive.eject", ui::AiIcon::Eject},
        {"drive.disconnect", ui::AiIcon::Plug}, {"preview.cloud_download", ui::AiIcon::Cloud},
    };
    for (const Pair& p : kExact)
        if (p.id == id) return p.icon;
    if (id.starts_with("tag.")) return ui::AiIcon::Tag;
    if (id.starts_with("tray.")) return ui::AiIcon::Tray;
    if (id.starts_with("search.")) return ui::AiIcon::Search;
    if (id.starts_with("keymap.")) return ui::AiIcon::Keyboard;
    if (id.starts_with("git.")) return ui::AiIcon::Git;
    if (id.starts_with("cloud.")) return ui::AiIcon::Cloud;
    return ui::AiIcon::Bolt;
}

// Organize rows (tags, aliases, the tray, recent, smart views): the card
// says 只改 Pulse 的整理信息.
bool ArgOrganizes(std::string_view id) {
    return id.starts_with("tag.") || id.starts_with("alias.") || id.starts_with("tray.") || id == "recent.remove" ||
           id == "smartview.save";
}

CtlStep CtlArgs(AppState& s, CtlRun& run, const std::string& id, const json::Value& act) {
    CtlStep st;
    st.icon = ArgIcon(id);
    st.title = StepTitle(s, id);
    app::actions::ArgRun r = app::actions::RunWithArgs(s, id, act);
    if (!r.title.empty()) st.title = std::move(r.title);   // Git 状态
    st.arg = std::move(r.arg);
    st.sub = std::move(r.sub);
    st.data = std::move(r.data);
    if (!r.ok) {
        st.error = r.error.empty() ? std::string("Pulse could not do it now") : r.error;
        st.why = r.why.empty() ? Text(I::AiActWhyFailed) : r.why;
        return st;
    }
    st.ok = true;
    st.skip = r.skip;
    if (r.skip || r.read_only) return st;
    // Git, shell items, programs, drives: the effect is outside Pulse, so no
    // 文件没动 line under the result.
    if (app::actions::HasAskText(id)) run.touched = true;
    ui::AiResultLine ln;
    ln.before = std::move(r.line_before);
    ln.bold = std::move(r.line_bold);
    ln.after = std::move(r.line_after);
    ln.caps_from = std::move(r.caps_from);
    ln.caps_to = std::move(r.caps_to);
    if (ln.before.empty() && ln.bold.empty() && ln.after.empty()) ln.before = st.arg.empty() ? st.title : st.title + L" \u00B7 " + st.arg;
    // The same action again, right after itself: one line.
    const bool follows = run.arg_last_id == id && run.arg_last_line + 1 == run.lines.size();
    const ui::AiResultLine* prev = follows ? &run.lines.back() : nullptr;
    int64_t tabs = 0;
    if (const json::Value* n = st.data.is_object() ? st.data.find("tabs") : nullptr) n->get_int(tabs, 0, 1000);
    const bool pin = st.data.is_object() && st.data.get_bool("pin", false);
    if (prev && prev->before == ln.before && prev->bold == ln.bold && prev->after == ln.after && ln.caps_to.empty()) {
        // 标签页颜色 无 → 蓝色 again: said already.
    } else if (prev && id == "tab.pin" && tabs > 0 && pin == run.arg_last_pin) {
        run.arg_tally += static_cast<size_t>(tabs);
        const std::wstring what = Fill(Text(I::AiActNTabs), L"{n}", Num(run.arg_tally));
        ui::AiResultLine merged = SplitLine(Fill(Text(pin ? I::AiActLinePinned : I::AiActLineUnpinned), L"{tabs}", what), what, what);
        run.single_title = merged.before + merged.bold + merged.after;
        run.lines.back() = std::move(merged);
    } else {
        run.arg_tally = static_cast<size_t>((std::max)(int64_t{1}, tabs));
        run.arg_last_pin = pin;
        if (ArgOrganizes(id)) {
            ++run.organize;
            run.organize_kinds.push_back(st.title);
            AddLine(run, std::move(ln));
        } else {
            PlainDone(run, std::move(ln));
        }
    }
    run.arg_last_id = id;
    run.arg_last_line = run.lines.size() - 1;
    if (id == "tag.create") run.arg_new_tag = st.arg;
    if (r.undo) run.undo.push_back(std::move(r.undo));
    if (id == "keymap.set") {
        // 新建文件夹的快捷键改好了 / 只影响 Pulse, 在设置中查看.
        run.title_override = Fill(Text(I::AiActKeysDone), L"{title}", st.arg);
        run.sub_override = Text(I::AiActKeymapSub);
        run.settings_page = 7;
    } else {
        AddPaletteKey(run, st.title);
    }
    return st;
}

// Rows that open a choose-yourself UI (the selection's context menu, 自定义
// 工具栏): Pulse opens it and the user picks; the assistant never picks for
// them (P4 decision 6).
bool IsChooser(std::string_view id) {
    return id == "file.context_menu" || id == "toolbar.customize" || id == "toolbar.pin_app" || id == "toolbar.tools";
}

CtlStep CtlChooser(AppState& s, const std::string& id) {
    CtlStep st;
    st.icon = id == "file.context_menu" ? ui::AiIcon::Rows : ui::AiIcon::Sliders;
    const auto views = app::actions::ListActions(s);
    const auto v = std::find_if(views.begin(), views.end(),
                                [&](const app::actions::ActionView& w) { return w.id == id && w.key.empty(); });
    if (v == views.end()) {
        st.title = Display(id);
        st.error = "unknown action (find_actions lists them)";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    st.title = v->title;
    if (!v->enabled) {
        st.error = "not available now: " + Utf8(v->why);
        st.why = v->why;
        return st;
    }
    // A menu runs a modal loop: open it once this step is over, not inside it.
    const int index = v->index;
    const HWND hwnd = s.hwnd;
    const bool posted = app::ai::PostAiUiTask(ServiceWindow(s), [index, hwnd](AppState* target) {
        if (target && target->hwnd == hwnd) app::actions::RunAction(*target, index);
    });
    if (!posted) {
        st.error = "Pulse could not open it now";
        st.why = Text(I::AiActWhyFailed);
        return st;
    }
    st.ok = true;
    st.sub = Text(I::AiActOpenedForYou);
    st.data = json::Value::Object();
    st.data.set("opened", true);
    st.data.set("note", "Pulse opened it for the user to choose; you cannot pick in it for them");
    return st;
}
} // namespace ai_agent

} // namespace pulse
