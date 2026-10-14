// session_window_test.cpp - Multi-window M0 baseline: the single-window session
// (window rect, maximized, layout tabs, tab groups, active tab) round-trips
// through SessionToJson / ParseSessionJson, and a frozen version-7 file keeps
// parsing to the same window. Session v8 (M4d, several windows) keeps these
// passing: old files migrate to windows[0], and the single-window fields that
// older versions read stay ahead of windows[] in the file.
#include "../app/session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace pulse::app;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

SessionSnapshot SampleSession() {
    SessionSnapshot s;
    s.window_rect = {120, 80, 1720, 1040};
    s.maximized = true;
    s.details_panel = true;
    s.details_panel_width = 360;

    LayoutTabSnapshot docs;
    docs.title = L"文档";
    docs.pinned = true;
    docs.group = 1;
    docs.layout = 0;
    PaneFolderSnapshot docs_pane;
    docs_pane.path = L"C:\\Users\\Example\\Documents";
    docs.panes.push_back(docs_pane);

    LayoutTabSnapshot work;
    work.title = L"";
    work.group = 1;
    work.layout = 1;
    work.focused = 1;
    work.split_ratios = {0.4f};
    PaneFolderSnapshot left, right;
    left.path = L"D:\\项目\\设计稿";
    left.view = pulse::ui::ViewMode::Details;
    right.path = L"\\\\nas\\share\\交付";
    work.panes = {left, right};

    s.layout_tabs = {docs, work};
    GroupSessionSnapshot group;
    group.id = 1;
    group.name = L"工作";
    group.color_rgb = 0x4CC2FF;
    s.tab_groups = {group};
    s.active_layout_tab = 1;
    return s;
}

void ExpectSampleWindow(const SessionSnapshot& got, const char* label) {
    std::string prefix = std::string(label) + ": ";
    auto check = [&](bool ok, const char* what) { Check(ok, (prefix + what).c_str()); };
    check(SameRect(got.window_rect, RECT{120, 80, 1720, 1040}), "window rect");
    check(got.maximized, "maximized");
    check(got.details_panel && got.details_panel_width == 360, "details panel state");
    check(got.layout_tabs.size() == 2, "two layout tabs");
    if (got.layout_tabs.size() != 2) return;
    const auto& docs = got.layout_tabs[0];
    const auto& work = got.layout_tabs[1];
    check(docs.title == L"文档" && docs.pinned && docs.group == 1, "first tab title, pin, group");
    check(docs.panes.size() == 1 && docs.panes[0].path == L"C:\\Users\\Example\\Documents",
          "first tab pane path");
    check(work.layout == 1 && work.focused == 1 && work.group == 1, "second tab layout, focus, group");
    check(work.panes.size() == 2 && work.panes[0].path == L"D:\\项目\\设计稿" &&
              work.panes[1].path == L"\\\\nas\\share\\交付",
          "second tab Unicode and UNC pane paths");
    check(work.split_ratios.size() == 1 && work.split_ratios[0] > 0.39f && work.split_ratios[0] < 0.41f,
          "split ratio");
    check(got.tab_groups.size() == 1 && got.tab_groups[0].id == 1 && got.tab_groups[0].name == L"工作" &&
              got.tab_groups[0].color_rgb == 0x4CC2FF,
          "tab group");
    check(got.active_layout_tab == 1, "active layout tab");
}

// A file without windows[] becomes one window holding the single-window fields.
void ExpectMigratedWindow(const SessionSnapshot& got, const char* label) {
    std::string prefix = std::string(label) + ": ";
    auto check = [&](bool ok, const char* what) { Check(ok, (prefix + what).c_str()); };
    check(got.windows.size() == 1 && got.focused_window == 0, "migrates to one window");
    if (got.windows.size() != 1) return;
    const WindowSessionSnapshot& w = got.windows[0];
    check(SameRect(w.window_rect, got.window_rect) && w.maximized == got.maximized,
          "window[0] rect and maximized");
    check(w.layout.details_panel == got.details_panel &&
              w.layout.details_panel_width == got.details_panel_width &&
              w.layout.sidebar_order == got.sidebar_order,
          "window[0] layout");
    check(w.layout_tabs.size() == got.layout_tabs.size() &&
              w.tab_groups.size() == got.tab_groups.size() &&
              w.active_layout_tab == got.active_layout_tab,
          "window[0] tabs, groups, active tab");
}

WindowSessionSnapshot WindowOf(const SessionSnapshot& s) {
    WindowSessionSnapshot w;
    w.window_rect = s.window_rect;
    w.maximized = s.maximized;
    w.layout.details_panel = s.details_panel;
    w.layout.details_panel_width = s.details_panel_width;
    w.layout_tabs = s.layout_tabs;
    w.tab_groups = s.tab_groups;
    w.active_layout_tab = s.active_layout_tab;
    return w;
}

// Three windows; the focused one (index 1) is SampleSession() and the
// single-window fields mirror it, as the app writes them. windows[0] differs
// in every checked field, so a top-level lookup that hit it would show.
SessionSnapshot SampleThreeWindows() {
    SessionSnapshot s = SampleSession();
    WindowSessionSnapshot recent;
    recent.window_rect = {2100, 140, 3300, 980};  // on a monitor right of the primary
    recent.layout.sidebar_hidden = 4;
    recent.layout.quick_access_hidden = 3;
    recent.layout.starred_expanded = false;
    recent.layout.details_panel_width = 400;
    LayoutTabSnapshot downloads;
    downloads.title = L"下载";
    PaneFolderSnapshot pane;
    pane.path = L"E:\\Downloads";
    downloads.panes = {pane};
    recent.layout_tabs = {downloads};
    recent.identity = {L"\u8BBE\u8BA1 \"\u7A3F\" \\ v2", 3};  // M7c: a typed name that needs escaping
    WindowSessionSnapshot third;
    third.window_rect = {300, 200, 1800, 1100};
    third.maximized = true;
    third.layout.details_panel = true;
    third.layout.details_preview = false;
    third.layout_tabs = {downloads, downloads};
    third.active_layout_tab = 1;
    s.windows = {recent, WindowOf(s), third};
    s.focused_window = 1;
    return s;
}

// ASCII-only C++ wide literal of `text` (one L"..." chunk per line), used to
// regenerate session_window_v7_fixture.inc with PULSE_SESSION_DUMP=1.
std::string ToWideLiteral(const std::wstring& text) {
    std::string out = "L\"";
    size_t column = 0;
    for (wchar_t ch : text) {
        char buf[12];
        if (ch == L'\\' || ch == L'"') std::snprintf(buf, sizeof(buf), "\\%c", static_cast<char>(ch));
        else if (ch == L'\n') std::snprintf(buf, sizeof(buf), "\\n");
        else if (ch >= 0x20 && ch < 0x7F) std::snprintf(buf, sizeof(buf), "%c", static_cast<char>(ch));
        else if (ch < 0xA0) std::snprintf(buf, sizeof(buf), "\\%03o", static_cast<unsigned>(ch));
        else if (ch >= 0xD800 && ch <= 0xDFFF)  // no \u for surrogates; end the chunk after \x
            std::snprintf(buf, sizeof(buf), "\\x%04X\" L\"", static_cast<unsigned>(ch));
        else std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(ch));
        out += buf;
        column += std::strlen(buf);
        if (ch == L'\n' || column > 90) { out += "\"\n    L\""; column = 0; }
    }
    out += "\"\n";
    return out;
}

// Written by SessionToJson at main 11a62b2 (session version 7) for
// SampleSession(); regenerate only together with a deliberate format change.
const wchar_t kFrozenV7[] =
#include "session_window_v7_fixture.inc"
    ;

}  // namespace

int main() {
    const SessionSnapshot sample = SampleSession();
    const std::wstring json = SessionToJson(sample);
    wchar_t dump[4]{};
    // PULSE_SESSION_DUMP=1: the writer's output (session_window_v7_fixture.inc);
    // =2: that output parsed and written again.
    if (GetEnvironmentVariableW(L"PULSE_SESSION_DUMP", dump, ARRAYSIZE(dump)) == 1) {
        SessionSnapshot reparsed;
        if (dump[0] == L'2') ParseSessionJson(json, reparsed);
        std::fputs(ToWideLiteral(dump[0] == L'2' ? SessionToJson(reparsed) : json).c_str(), stdout);
        return 0;
    }

    Check(json.find(L"\"version\":8") != std::wstring::npos, "writer emits session version 8");
    Check(json.find(L"\"windows\"") == std::wstring::npos,
          "writer leaves windows[] out when the snapshot has none");
    SessionSnapshot parsed;
    Check(ParseSessionJson(json, parsed), "round-trip JSON parses");
    ExpectSampleWindow(parsed, "round-trip");
    ExpectMigratedWindow(parsed, "round-trip");

    SessionSnapshot frozen;
    Check(ParseSessionJson(kFrozenV7, frozen), "frozen v7 file parses");
    ExpectSampleWindow(frozen, "frozen v7");
    ExpectMigratedWindow(frozen, "frozen v7");
    // The parser normalizes defaults (an empty sidebarOrder becomes the default
    // order), so compare the rewritten forms rather than the raw writer output.
    Check(SessionToJson(frozen) == SessionToJson(parsed),
          "frozen v7 file and the current writer parse to the same state");

    SessionSnapshot empty;
    Check(ParseSessionJson(SessionToJson(SessionSnapshot{}), empty) && empty.layout_tabs.empty() &&
              !empty.maximized,
          "empty session round-trips as an empty window");

    // Version 8: several windows.
    const std::wstring multi = SessionToJson(SampleThreeWindows());
    const size_t windows_at = multi.find(L"\"windows\"");
    Check(windows_at != std::wstring::npos && multi.find(L"\"layoutTabs\"") < windows_at &&
              multi.find(L"\"left\"") < windows_at && multi.find(L"\"activeTab\"") < windows_at,
          "v8: single-window fields come before windows[]");
    SessionSnapshot three;
    Check(ParseSessionJson(multi, three), "v8: three-window JSON parses");
    ExpectSampleWindow(three, "v8 single-window fields");
    Check(three.windows.size() == 3 && three.focused_window == 1, "v8: three windows, focused index 1");
    if (three.windows.size() == 3) {
        const WindowSessionSnapshot& a = three.windows[0];
        const WindowSessionSnapshot& c = three.windows[2];
        Check(SameRect(a.window_rect, RECT{2100, 140, 3300, 980}) && !a.maximized,
              "v8: window 0 rect on the second monitor");
        Check(a.layout.sidebar_hidden == 4 && a.layout.quick_access_hidden == 3 &&
                  !a.layout.starred_expanded && a.layout.details_panel_width == 400,
              "v8: window 0 layout");
        Check(a.layout_tabs.size() == 1 && a.layout_tabs[0].title == L"下载" &&
                  a.layout_tabs[0].panes.size() == 1 &&
                  a.layout_tabs[0].panes[0].path == L"E:\\Downloads",
              "v8: window 0 tab and pane path");
        const WindowSessionSnapshot& b = three.windows[1];
        Check(SameRect(b.window_rect, RECT{120, 80, 1720, 1040}) && b.maximized &&
                  b.layout.details_panel && b.layout.details_panel_width == 360 &&
                  b.layout_tabs.size() == 2 && b.layout_tabs[1].panes.size() == 2 &&
                  b.layout_tabs[1].panes[1].path == L"\\\\nas\\share\\交付" &&
                  b.tab_groups.size() == 1 && b.active_layout_tab == 1,
              "v8: focused window 1 round-trips");
        Check(SameRect(c.window_rect, RECT{300, 200, 1800, 1100}) && c.maximized &&
                  c.layout.details_panel && !c.layout.details_preview &&
                  c.layout_tabs.size() == 2 && c.active_layout_tab == 1,
              "v8: window 2 maximized, layout, two tabs, active tab");
        Check(a.identity.name == L"\u8BBE\u8BA1 \"\u7A3F\" \\ v2" && a.identity.color == 3,
              "v8: a typed window name (quotes, backslash) and its color round-trip");
        Check(b.identity == WindowIdentity{} && c.identity == WindowIdentity{},
              "v8: an automatic name and no color yet round-trip as such");
    }
    // The parser normalizes defaults (empty sidebarOrder), so compare after one pass.
    SessionSnapshot again;
    Check(ParseSessionJson(SessionToJson(three), again) && SessionToJson(again) == SessionToJson(three),
          "v8: parse and write again gives the same file");

    // What a version-7 reader (1.0.53) gets from a v8 file: it ignores
    // windows[], so it sees the single-window fields, i.e. the focused window.
    SessionSnapshot old_reader;
    const size_t tail = multi.find(L",\n  \"focusedWindow\"");
    Check(tail != std::wstring::npos &&
              ParseSessionJson(multi.substr(0, tail) + L"\n}\n", old_reader),
          "v8 without windows[] still parses");
    ExpectSampleWindow(old_reader, "v8 read as v7");

    // quickAccessHidden keeps its version-7 gate (not "current version").
    SessionSnapshot qa7, qa6;
    Check(ParseSessionJson(L"{\"version\":7,\"quickAccessHidden\":5}", qa7) &&
              qa7.quick_access_hidden == 5 && qa7.windows.size() == 1 &&
              qa7.windows[0].layout.quick_access_hidden == 5,
          "v7 quick-access mask survives the v8 upgrade");
    Check(ParseSessionJson(L"{\"version\":6,\"quickAccessHidden\":5}", qa6) &&
              qa6.quick_access_hidden == 0,
          "v6 quick-access mask is still dropped");

    SessionSnapshot bad_windows;
    Check(ParseSessionJson(L"{\"version\":8,\"left\":10,\"right\":20,\"windows\":[1,2]}", bad_windows) &&
              bad_windows.windows.size() == 1 && bad_windows.windows[0].window_rect.right == 20,
          "windows[] without objects falls back to the single window");
    SessionSnapshot wide;
    Check(ParseSessionJson(L"{\"version\":8,\"windows\":[{\"left\":1,\"detailsPanelWidth\":9999}]}", wide) &&
              wide.windows.size() == 1 && wide.windows[0].layout.details_panel_width == 340 &&
              wide.windows[0].layout.details_preview && wide.windows[0].layout.starred_expanded,
          "window with an out-of-range width or missing keys gets the defaults");
    Check(wide.windows.size() == 1 && wide.windows[0].identity == WindowIdentity{},
          "M7c: a window saved before names and colors gets an automatic name and a fresh color");
    SessionSnapshot odd;
    Check(ParseSessionJson(L"{\"version\":8,\"windows\":[{\"windowName\":\"  Work\\t \",\"windowColor\":9,"
                           L"\"tabGroups\":[{\"id\":1,\"name\":\"g\",\"color\":2}]}]}", odd) &&
              odd.windows.size() == 1 && odd.windows[0].identity.name == L"Work" &&
              odd.windows[0].identity.color == -1 && odd.windows[0].tab_groups.size() == 1,
          "M7c: a saved name is trimmed, an unknown color dropped, groups untouched");

    // M7f: closed windows survive a restart (plan §7, at most kSessionClosedWindows).
    {
        SessionSnapshot with_closed = SampleSession();
        WindowSessionSnapshot open_window;
        open_window.window_rect = {5, 6, 705, 506};
        open_window.layout_tabs = with_closed.layout_tabs;
        with_closed.windows.push_back(open_window);
        for (int i = 0; i < 7; ++i) {
            ClosedWindowSessionSnapshot closed;
            closed.name = L"窗口 \"" + std::to_wstring(i) + L"\"";
            closed.window.window_rect = {-1900 + i, 100, -900 + i, 800};  // a screen rect, left monitor
            closed.window.maximized = i == 6;
            closed.window.layout.details_panel = true;
            closed.window.layout.details_panel_width = 300 + i;
            closed.window.layout_tabs = with_closed.layout_tabs;
            closed.window.layout_tabs.resize(1 + static_cast<size_t>(i % 2));
            closed.window.tab_groups = with_closed.tab_groups;
            closed.window.active_layout_tab = i % 2;
            closed.window.identity = {L"Work " + std::to_wstring(i), i % 6};
            with_closed.closed_windows.push_back(closed);
        }
        const std::wstring closed_json = SessionToJson(with_closed);
        Check(closed_json.find(L"\"closedWindows\"") > closed_json.find(L"\"windows\""),
              "M7f: closedWindows[] is written after windows[]");
        SessionSnapshot back;
        Check(ParseSessionJson(closed_json, back), "M7f: a session with closed windows parses");
        Check(back.closed_windows.size() == kSessionClosedWindows,
              "M7f: only the newest five closed windows are kept");
        bool same = back.closed_windows.size() == kSessionClosedWindows;
        for (size_t k = 0; same && k < back.closed_windows.size(); ++k) {
            const ClosedWindowSessionSnapshot& want = with_closed.closed_windows[k + 2];
            const ClosedWindowSessionSnapshot& got = back.closed_windows[k];
            same = got.name == want.name && SameRect(got.window.window_rect, want.window.window_rect) &&
                   got.window.maximized == want.window.maximized &&
                   got.window.layout.details_panel_width == want.window.layout.details_panel_width &&
                   got.window.layout_tabs.size() == want.window.layout_tabs.size() &&
                   got.window.layout_tabs[0].title == want.window.layout_tabs[0].title &&
                   got.window.tab_groups.size() == want.window.tab_groups.size() &&
                   got.window.active_layout_tab == want.window.active_layout_tab &&
                   got.window.identity == want.window.identity;
        }
        Check(same, "M7f: each closed window keeps its name, screen rect, layout, tabs, groups and identity, oldest first");
        Check(back.windows.size() == 1 && SameRect(back.windows[0].window_rect, open_window.window_rect) &&
                  back.windows[0].layout_tabs.size() == open_window.layout_tabs.size(),
              "M7f: windows[] is unaffected by closedWindows[]");
        ExpectSampleWindow(back, "with closedWindows");

        SessionSnapshot no_closed;
        const size_t cut = closed_json.find(L",\n  \"closedWindows\"");
        Check(cut != std::wstring::npos && ParseSessionJson(closed_json.substr(0, cut) + L"\n}\n", no_closed) &&
                  no_closed.closed_windows.empty() && no_closed.windows.size() == 1,
              "M7f: a session without closedWindows[] has no closed windows");
        SessionSnapshot v7;
        Check(ParseSessionJson(kFrozenV7, v7) && v7.closed_windows.empty(),
              "M7f: the frozen v7 file has no closed windows");

        SessionSnapshot empty_tabs;
        Check(ParseSessionJson(L"{\"version\":8,\"closedWindows\":[{\"left\":1,\"closedName\":\"x\"},"
                               L"{\"left\":2,\"closedName\":\"y\",\"layoutTabs\":[{\"title\":\"t\",\"layout\":0,"
                               L"\"panes\":[{\"path\":\"C:\\\\\"}]}]}]}", empty_tabs) &&
                  empty_tabs.closed_windows.size() == 1 && empty_tabs.closed_windows[0].name == L"y" &&
                  empty_tabs.closed_windows[0].window.window_rect.left == 2,
              "M7f: a closed window without tabs is dropped");
        Check(SessionToJson(SampleSession()).find(L"closedWindows") == std::wstring::npos,
              "M7f: no closedWindows[] when nothing was closed");
    }

    {
        // The group chip hover card: a folded group lists its members (the
        // active one marked) followed by the two action rows; an expanded group
        // already shows its tabs in the strip, so the card keeps only the
        // actions.
        WindowTabs card_tabs;
        std::vector<GroupSessionSnapshot> card_groups{
            { 4, L"work", 0x00112233, true },
        };
        LayoutTabSnapshot card_member;
        card_member.group = 4;
        card_member.panes.push_back({ L"C:\\Windows", pulse::ui::ViewMode::Details, {}, {} });
        RestoreWindowTabs(card_tabs, { card_member }, card_groups, 0,
            [](Tab& tab, const std::wstring& loaded) { tab.current_path = loaded; });
        const auto rows = TabGroupCardRows(card_tabs, 4, nullptr);
        Check(rows.size() == 3 && rows[0].tab_index == 0 && rows[0].active &&
                  rows[1].new_tab && rows[2].edit,
              "group card: a folded group lists its member and the two actions");
        Check(rows[0].separator_after,
              "group card: the member run carries the divider above the actions");
        card_tabs.tab_groups[0].collapsed = false;
        const auto expanded = TabGroupCardRows(card_tabs, 4, nullptr);
        Check(expanded.size() == 2 && expanded[0].new_tab && expanded[1].edit,
              "group card: an expanded group keeps only the actions");
        Check(TabGroupCardRows(card_tabs, 5, nullptr).empty(),
              "group card: an unknown group shows nothing");
    }

    std::printf("session_window_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
