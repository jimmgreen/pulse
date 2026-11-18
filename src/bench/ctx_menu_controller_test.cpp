// ctx_menu_controller_test.cpp - ContextMenuController bookkeeping: the
// per-menu timing record (ctx_menu_timing.h), static-verb re-reads, rows
// painted from the cache while the live answer arrives in parts, and the
// persistent layout with templated names (ctx_layout_cache.h), and the
// per-handler shard merge driven by the host's handler states (C4b), and the
// open menu's fixed shape: late rows greyed in place or put in 更多扩展 (R3).
#include "../app/context_menu_controller.h"
#include "../app/ctx_layout_cache.h"
#include "../app/ctx_menu_timing.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

pulse::ops::ShellMenuItem Item(uint32_t id, const wchar_t* text) {
    pulse::ops::ShellMenuItem item;
    item.id = id;
    item.text = text;
    return item;
}

pulse::ops::ShellMenuItem Row(uint32_t id, const wchar_t* clsid, const wchar_t* verb,
                              const wchar_t* text, bool parent = false, bool child = false) {
    pulse::ops::ShellMenuItem item;
    item.id = id;
    item.clsid = clsid;
    item.verb = verb;
    item.text = text;
    item.has_children = parent;
    item.child = child;
    return item;
}

const pulse::ops::ShellMenuItem* RowNamed(const std::vector<pulse::ops::ShellMenuItem>& items,
                                          const wchar_t* text) {
    for (const auto& item : items)
        if (item.text == text) return &item;
    return nullptr;
}

pulse::ops::ShellMenuHandlerStatus Handler(const wchar_t* clsid, bool done, uint32_t ms,
                                           uint32_t rows = 0) {
    pulse::ops::ShellMenuHandlerStatus status;
    status.clsid = clsid;
    status.done = done;
    status.elapsed_ms = ms;
    status.rows = rows;
    return status;
}

std::wstring Texts(const std::vector<pulse::ops::ShellMenuItem>& items) {
    std::wstring out;
    for (const auto& item : items) out += item.text + L"|";
    return out;
}

bool Predicted(uint32_t id) {
    return id >= pulse::app::ContextMenuController::kPredictedIdFirst &&
           id <= pulse::app::ContextMenuController::kPredictedIdLast;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

pulse::ui::FluentMenuItem MenuRow(int command, const wchar_t* text, bool separator = false) {
    pulse::ui::FluentMenuItem row;
    row.command = command;
    row.text = text;
    row.separator_after = separator;
    return row;
}

pulse::ui::FluentMenuItem MenuFlyout(const wchar_t* text, std::vector<int> commands) {
    pulse::ui::FluentMenuItem row;
    row.text = text;
    for (int command : commands)
        row.children.push_back(MenuRow(command, (L"c" + std::to_wstring(command)).c_str()));
    return row;
}

bool SameShape(const std::vector<pulse::ui::FluentMenuItem>& a,
               const std::vector<pulse::ui::FluentMenuItem>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].separator_after != b[i].separator_after) return false;
    return true;
}

int RowIndex(const std::vector<pulse::ui::FluentMenuItem>& rows, const wchar_t* text) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].text == text) return static_cast<int>(i);
    return -1;
}

} // namespace

#include "shell_action_routing_test.inc"
#include "nested_menu_controller_test.inc"

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--nested-only") return NestedMenuControllerTests();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--shell-actions") return ShellActionRoutingTests();
    using namespace pulse;
    app::ContextMenuPrefs prefs;
    prefs.persist = false;

    auto start = [&](app::ContextMenuController& controller) {
        app::ContextMenuController::ShellOperations operations;
        operations.query = [](std::vector<std::wstring>, HWND, bool, bool,
                              std::vector<std::wstring>) { return 7u; };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, nullptr, { L"C:\\fixture\\a.dwg" }, false, L".dwg",
                              false, {}, {});
    };

    {
        // Menu opened before the host answered; the answer grows it once.
        app::ContextMenuController controller;
        start(controller);
        controller.OpenMenu({});
        controller.BeginTiming(12, 5, false, 1000);
        controller.NoteDisplayUpdate(5, 1040);   // ids patched, same rows
        controller.CompleteComQuery(7, { Item(1, L"A"), Item(2, L"B") }, 1180, false);
        controller.NoteDisplayUpdate(7, 1180);   // rows added on screen
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(true, timing), "a begun record is taken once");
        Check(timing.key == L".dwg" && !timing.background, "record names the menu's type");
        Check(timing.input_to_open_ms == 12 && timing.rows_open == 5 && !timing.seeded,
              "record keeps the open-time facts");
        Check(!timing.live_at_open && timing.live_after_open_ms == 180,
              "final answer after open is timed from the open");
        Check(timing.refreshes == 2 && timing.grew == 1 && timing.rows_final == 7,
              "only an update that changes the row count counts as growth");
        Check(timing.shell_command, "record keeps whether an Explorer row ran");
        Check(timing.trace == "40:5 180:7", "trace lists every rebuild as ms-since-open:rows");
        app::CtxMenuTiming again;
        Check(!controller.TakeTiming(false, again), "a record is not taken twice");
    }
    {
        // Answer in before the popup: nothing may grow.
        app::ContextMenuController controller;
        start(controller);
        controller.CompleteComQuery(7, { Item(1, L"A") }, 900, false);
        controller.OpenMenu({});
        controller.BeginTiming(3, 6, true, 1000);
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(false, timing) && timing.live_at_open &&
                  timing.live_after_open_ms == -1 && timing.grew == 0 && timing.rows_final == 6,
              "an answer that beat the popup is recorded as live at open");
    }
    {
        // Partial answers never stamp the final arrival.
        app::ContextMenuController controller;
        start(controller);
        controller.OpenMenu({});
        controller.BeginTiming(0, 4, false, 1000);
        controller.CompleteComQuery(7, { Item(1, L"A") }, 1080, true);
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(false, timing) && timing.live_after_open_ms == -1,
              "a partial snapshot is not the final answer");
    }
    {
        // Re-reading a cached type: throttled, and a re-read that finds the same
        // verbs must not touch an open menu.
        app::ContextMenuController controller;
        start(controller);
        const app::StaticVerb edit{ L"edit", L"Edit", L"", L"", {}, 0 };
        const app::StaticVerb print{ L"print", L"Print", L"", L"", {}, 0 };
        Check(!controller.RequestStaticRevalidate(L".dwg", 0),
              "an uncached type is fetched, not re-read");
        Check(controller.RequestStaticPrefetch(L".dwg"), "first fetch is requested");
        Check(!controller.RequestStaticRevalidate(L".dwg", 0), "no re-read while a read runs");
        Check(controller.CompleteStaticVerbs(L".dwg", { edit }, controller.cache_generation()),
              "first rows change the menu");
        Check(!controller.RequestStaticPrefetch(L".dwg"), "a cached type is not fetched again");
        Check(controller.RequestStaticRevalidate(L".dwg", 10000), "a cached type is re-read");
        Check(!controller.CompleteStaticVerbs(L".dwg", { edit }, controller.cache_generation()),
              "a re-read with the same verbs changes nothing");
        Check(!controller.RequestStaticRevalidate(
                  L".dwg", 10000 + app::ContextMenuController::kStaticRevalidateMs - 1),
              "re-reads are throttled");
        Check(controller.RequestStaticRevalidate(
                  L".dwg", 10000 + app::ContextMenuController::kStaticRevalidateMs),
              "the next re-read is allowed after the interval");
        Check(controller.CompleteStaticVerbs(L".dwg", { edit, print },
                                             controller.cache_generation()) &&
                  controller.HasCachedStaticVerbs(L".dwg"),
              "a re-read that finds a new verb updates the menu and the cache");
        const uint32_t before = controller.cache_generation();
        Check(controller.RequestStaticRevalidate(L".dwg", 100000), "re-read before a flush");
        controller.InvalidateCaches();
        Check(!controller.CompleteStaticVerbs(L".dwg", { edit }, before) &&
                  !controller.HasCachedStaticVerbs(L".dwg"),
              "a re-read from before a registry flush is dropped");
        Check(!controller.RequestStaticRevalidate(L".dwg", 100001),
              "after a flush the type is fetched afresh, not re-read");
    }
    {
        // Painted from the cache, then the live answer in parts.
        app::ContextMenuController controller;
        uint32_t token = 10;
        uint32_t invoked = 0;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++token; };
        operations.invoke = [&](uint32_t, uint32_t id, std::wstring, std::wstring) {
            invoked = id;
        };
        controller.SetShellOperations(std::move(operations));
        const std::vector<ops::ShellMenuItem> complete = {
            Row(3, L"{A}", L"", L"Zip", true), Row(4, L"{A}", L"", L"to a.zip", false, true),
            Row(5, L"{A}", L"", L"to a.7z", false, true), Row(9, L"{B}", L"scan", L"Scan"),
            Row(12, L"{C}", L"", L"Send") };
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.dwg" }, false, L".dwg", false, {}, {});
        controller.CompleteComQuery(token, complete, 100, false);
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\b.dwg" }, false, L".dwg", false, {}, {});
        Check(controller.com_predicted() && controller.com_items().size() == complete.size(),
              "the next query of the type starts from the last complete answer");
        Check(!controller.SeedComItemsFromCache(), "seeding again at popup time changes nothing");
        bool synthetic = true;
        for (size_t i = 0; i < controller.com_items().size(); ++i) {
            const uint32_t id = controller.com_items()[i].id;
            synthetic &= Predicted(id) && (i == 0 || id != controller.com_items()[i - 1].id);
        }
        Check(synthetic, "painted rows carry their own ids, never the last menu's host ids");

        controller.CompleteComQuery(token, { Row(31, L"{B}", L"scan", L"Scan"),
                                             Row(40, L"{D}", L"", L"Late handler") }, 150, true);
        const auto& first = controller.com_items();
        Check(first.size() == complete.size() && RowNamed(first, L"Scan") &&
                  RowNamed(first, L"Scan")->id == 31 && Predicted(RowNamed(first, L"Send")->id),
              "a partial answer patches the rows it names and keeps the others");
        Check(!RowNamed(first, L"Late handler"), "rows the cache lacks wait for the full answer");

        controller.CompleteComQuery(token, { Row(20, L"{A}", L"", L"Zip", true),
                                             Row(21, L"{A}", L"", L"to b.zip", false, true),
                                             Row(31, L"{B}", L"scan", L"Scan") }, 200, true);
        Check(controller.com_items().size() == complete.size() &&
                  RowNamed(controller.com_items(), L"Zip")->id == 20,
              "a later partial keeps the row count too");

        const uint32_t send = RowNamed(controller.com_items(), L"Send")->id;
        const std::vector<ops::ShellMenuItem> final_rows = {
            Row(20, L"{A}", L"", L"Zip", true), Row(21, L"{A}", L"", L"to b.zip", false, true),
            Row(22, L"{A}", L"", L"to b.7z", false, true), Row(31, L"{B}", L"scan", L"Scan"),
            Row(50, L"{C}", L"", L"Send") };
        controller.ExecuteShellCommand(
            static_cast<int>(app::CmdShellComBase + send),
            [&] { controller.CompleteComQuery(token, final_rows, 300, false); }, {});
        Check(invoked == 50, "a painted row clicked before the answer runs the live command");

        controller.StartQuery(prefs, nullptr, { L"C:\\f\\c.dwg" }, false, L".dwg", false, {}, {});
        controller.CompleteComQuery(token, final_rows, 400, false);
        Check(controller.com_items().size() == final_rows.size() &&
                  RowNamed(controller.com_items(), L"to b.7z")->id == 22 &&
                  !Predicted(RowNamed(controller.com_items(), L"Send")->id),
              "the full answer replaces the painted rows");

        controller.StartQuery(prefs, nullptr, { L"C:\\f\\d.zip" }, false, L".zip", false, {}, {});
        Check(!controller.com_predicted() && !controller.SeedComItemsFromCache(),
              "an unseen type has nothing to paint");
        controller.CompleteComQuery(token, { Row(7, L"{B}", L"scan", L"Scan") }, 500, true);
        Check(controller.com_items().size() == 1 && controller.com_items()[0].id == 7,
              "without cached rows a partial answer is shown as it is");
    }
    {
        app::CtxMenuTiming timing;
        timing.key = L".d\"w\\g\x4E2D";
        timing.rows_open = 9;
        timing.grew = 2;
        timing.trace = "12:9 80:7";
        const std::string line = app::FormatCtxMenuTiming(timing, 42);
        Check(!line.empty() && line.back() == '\n' && line.find('\n') == line.size() - 1,
              "one record is one line");
        Check(Contains(line, "\"event\":\"ctx_menu\"") && Contains(line, "\"tick_ms\":42") &&
                  Contains(line, "\"rows_open\":9") && Contains(line, "\"grew\":2") &&
                  Contains(line, "\"trace\":\"12:9 80:7\""),
              "record carries the counters");
        Check(Contains(line, "\"key\":\".d\\u0022w\\u005cg\\u4e2d\""),
              "quotes, backslashes and non-ASCII in the key stay valid JSON");
    }
    {
        // {name}: labels keep the item's names as markers.
        const auto single = app::CtxNamesFor({ L"C:\\f\\a.dwg" }, false);
        Check(single.file == L"a.dwg" && single.stem == L"a" && single.folder.empty(),
              "one item: file name and stem");
        const auto report = app::CtxNamesFor({ L"\\\\?\\D:\\docs\\report.txt" }, false);
        const std::wstring zip = app::TemplateCtxLabel(L"\x538B\x7F29\x5230 \"a.zip\"", single);
        Check(zip.find(app::kCtxNameStem) != std::wstring::npos &&
                  app::ExpandCtxLabel(zip, report) == L"\x538B\x7F29\x5230 \"report.zip\"",
              "a quoted stem becomes a marker and takes the next item's name");
        Check(app::TemplateCtxLabel(L"Bandizip", single) == L"Bandizip" &&
                  app::TemplateCtxLabel(L"Scan with a tool", single) == L"Scan with a tool" &&
                  app::TemplateCtxLabel(L"to a.zip", single).find(app::kCtxNameStem) == 3,
              "a name inside a word or a sentence stays; before an extension it is templated");
        const auto git = app::CtxNamesFor({ L"C:\\src\\Git" }, true);
        Check(git.file == L"Git" && git.stem == L"Git" &&
                  app::TemplateCtxLabel(L"Git Bash Here", git) == L"Git Bash Here" &&
                  app::TemplateCtxLabel(L"Open \"Git\" in Code", git).find(app::kCtxNameFile) == 6,
              "background menus template the folder only where it is quoted");
        const auto tar = app::CtxNamesFor({ L"C:\\f\\a.tar.gz" }, false);
        const std::wstring extract = app::TemplateCtxLabel(L"Extract to \"a.tar\\\"", tar);
        Check(extract == std::wstring(L"Extract to \"") + app::kCtxNameStem + L"\\\"",
              "the longest matching name wins");
        const auto multi = app::CtxNamesFor({ L"C:\\work\\x.txt", L"C:\\work\\y.txt" }, false);
        const auto other = app::CtxNamesFor({ L"C:\\other\\p.txt", L"C:\\other\\q.txt" }, false);
        Check(multi.file.empty() && multi.folder == L"work" &&
                  app::ExpandCtxLabel(app::TemplateCtxLabel(L"Add to \"work.zip\"", multi), other) ==
                      L"Add to \"other.zip\"",
              "several items: the folder they are in");
        const auto drive = app::CtxNamesFor({ L"C:\\" }, false);
        Check(drive.file.empty() && drive.stem.empty() &&
                  app::TemplateCtxLabel(L"C: \"C:\"", drive) == L"C: \"C:\"",
              "a drive root has no name to template");
    }
    {
        // Seeds take the current names; single and multi selections are apart.
        app::ContextMenuController controller;
        uint32_t token = 60;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++token; };
        controller.SetShellOperations(std::move(operations));
        const std::vector<ops::ShellMenuItem> rows = {
            Row(3, L"{A}", L"", L"Zip", true), Row(4, L"{A}", L"", L"to a.zip", false, true),
            Row(9, L"{B}", L"scan", L"Scan") };
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.dwg" }, false, L".dwg", false, {}, {});
        controller.CompleteComQuery(token, rows, 100, false);
        Check(!controller.com_layout_loaded() && controller.TakeComLayoutDirty() &&
                  !controller.TakeComLayoutDirty(),
              "a new layout is dirty once (and waits until the stored one is loaded)");
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\b.dwg" }, false, L".dwg", false, {}, {});
        Check(controller.com_predicted() && RowNamed(controller.com_items(), L"to b.zip") &&
                  !RowNamed(controller.com_items(), L"to a.zip"),
              "painted rows carry the clicked item's name, not the last one's");
        controller.CompleteComQuery(token, { Row(13, L"{A}", L"", L"Zip", true),
                                             Row(14, L"{A}", L"", L"to b.zip", false, true),
                                             Row(19, L"{B}", L"scan", L"Scan") }, 200, false);
        Check(!controller.TakeComLayoutDirty(), "the same layout for another name is not dirty");
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\c.dwg", L"C:\\f\\d.dwg" }, false,
                              L".dwg", false, {}, {});
        Check(!controller.com_predicted(), "several selected items do not reuse the single layout");
        controller.CompleteComQuery(token, { Row(5, L"{B}", L"scan", L"Scan") }, 300, false);
        const auto snapshot = controller.ComLayoutSnapshot();
        Check(snapshot.size() == 2 && snapshot.contains(L".dwg") && snapshot.contains(L".dwg|m") &&
                  snapshot.at(L".dwg").rows[1].text == std::wstring(L"to ") + app::kCtxNameStem + L".zip",
              "the snapshot holds both keys with templated labels");

        app::ContextMenuController loaded;
        uint32_t loaded_token = 80;
        app::ContextMenuController::ShellOperations loaded_ops;
        loaded_ops.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++loaded_token; };
        loaded.SetShellOperations(std::move(loaded_ops));
        loaded.StartQuery(prefs, nullptr, { L"C:\\f\\e.zip" }, false, L".zip", false, {}, {});
        loaded.CompleteComQuery(loaded_token, { Row(2, L"{Z}", L"", L"Fresh") }, 50, false);
        auto from_disk = snapshot;
        from_disk[L".zip"] = { { Row(1, L"{Z}", L"", L"Stale") } };
        loaded.StartQuery(prefs, nullptr, { L"C:\\f\\g.dwg" }, false, L".dwg", false, {}, {});
        Check(!loaded.com_predicted(), "nothing to paint before the layout is read");
        Check(loaded.MergeComLayout(from_disk) == 2 && loaded.com_layout_loaded(),
              "merging adds the keys this session has not answered");
        Check(loaded.com_predicted() && RowNamed(loaded.com_items(), L"to g.zip"),
              "the query waiting for the popup paints from the merged layout");
        loaded.StartQuery(prefs, nullptr, { L"C:\\f\\h.zip" }, false, L".zip", false, {}, {});
        Check(RowNamed(loaded.com_items(), L"Fresh") && !RowNamed(loaded.com_items(), L"Stale"),
              "a key answered this session is newer than the file");
        loaded.InvalidateCaches();
        Check(!loaded.com_layout_loaded() && loaded.ComLayoutSnapshot().empty(),
              "a registry flush empties the layout until it is read again");
    }
    {
        // A menu closed before the complete answer still learns the layout.
        app::ContextMenuController controller;
        uint32_t token = 90;
        std::vector<uint32_t> closed;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++token; };
        operations.close = [&](uint32_t id) { closed.push_back(id); };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.png" }, false, L".png", false, {}, {});
        const uint32_t first = token;
        controller.Close();
        Check(closed.empty(), "closing before the answer keeps the host session running");
        const auto late = controller.CompleteComQuery(first, { Row(3, L"{P}", L"", L"Print a.png") },
                                                      100, true);
        Check(!late.accepted && controller.ComLayoutSnapshot().empty(),
              "a closed menu's partial answer is dropped");
        const auto done = controller.CompleteComQuery(
            first, { Row(3, L"{P}", L"", L"Print \"a.png\""), Row(4, L"{Q}", L"", L"Rotate") }, 200, false);
        Check(!done.accepted && controller.TakeComLayoutDirty(),
              "its complete answer is stored (the caller closes the session)");
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\b.png" }, false, L".png", false, {}, {});
        Check(controller.com_predicted() && RowNamed(controller.com_items(), L"Print \"b.png\"") &&
                  controller.com_items().size() == 2,
              "the next menu of the type paints the late answer with its own name");
        const uint32_t second = token;
        controller.Close();
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\c.txt" }, false, L".txt", false, {}, {});
        const uint32_t third = token;
        controller.Close();
        Check(closed.size() == 1 && closed[0] == second,
              "one session drains at a time: the next one closes the previous");
        controller.InvalidateCaches();
        Check(closed.size() == 2 && closed[1] == third,
              "a registry flush closes the draining session");
        Check(!controller.CompleteComQuery(third, { Row(5, L"{T}", L"", L"Edit") }, 300, false).accepted &&
                  controller.ComLayoutSnapshot().empty(),
              "an answer read before the flush is not stored");
    }
    {
        // C4b shard merge, pure: finished handlers take their live rows,
        // unfinished ones keep the previous rows, menu order is the host's.
        const std::vector<ops::ShellMenuItem> previous = {
            Row(1, L"{A}", L"", L"Zip", true), Row(2, L"{A}", L"", L"to a.zip", false, true),
            Row(3, L"{B}", L"scan", L"Scan"), Row(4, L"{C}", L"", L"Gone soon"),
            Row(5, L"{X}", L"", L"Not queried any more") };
        const std::vector<ops::ShellMenuItem> live = {
            Row(11, L"{A}", L"", L"Zip", true), Row(12, L"{A}", L"", L"to b.zip", false, true),
            Row(13, L"{A}", L"", L"to b.7z", false, true), Row(14, L"{D}", L"", L"New") };
        const auto merged = app::MergeCtxHandlerShards(previous, live,
            { Handler(L"{A}", true, 20, 3), Handler(L"{B}", false, 80), Handler(L"{C}", true, 30),
              Handler(L"{D}", true, 40, 1) });
        Check(Texts(merged) == L"Zip|to b.zip|to b.7z|Scan|New|" && merged[3].id == 3 &&
                  merged[0].id == 11,
              "finished handlers bring their live rows, a running one keeps its rows in place");
        const auto all_done = app::MergeCtxHandlerShards(previous, live,
            { Handler(L"{A}", true, 20, 3), Handler(L"{B}", true, 90), Handler(L"{D}", true, 40, 1) });
        Check(Texts(all_done) == Texts(live), "with every handler finished the merge is the live answer");
        const auto lower = app::MergeCtxHandlerShards(previous, { Row(21, L"{Q}", L"", L"Unlisted") },
            { Handler(L"{b}", false, 80) });
        Check(Texts(lower) == L"Scan|Unlisted|", "CLSIDs match regardless of case; unlisted live rows stay");

        app::CtxHandlerStats stats;
        Check(app::MergeCtxHandlerStats(stats, { Handler(L"{A}", true, 30, 2), Handler(L"{B}", false, 80) }) &&
                  stats.size() == 2 && stats[0].done && stats[0].rows == 2 && !stats[1].done &&
                  stats[1].elapsed_ms == 80,
              "handler states are recorded in menu order");
        Check(!app::MergeCtxHandlerStats(stats, { Handler(L"{A}", true, 45, 2), Handler(L"{B}", false, 90) }) &&
                  stats[0].elapsed_ms == 45,
              "a time inside the same bucket is kept but does not ask for a save");
        Check(app::MergeCtxHandlerStats(stats, { Handler(L"{A}", true, 130, 2), Handler(L"{B}", false, 95) }),
              "a time in another bucket asks for a save");
        Check(app::MergeCtxHandlerStats(stats, { Handler(L"{A}", false, 60), Handler(L"{B}", true, 700, 1) }) &&
                  stats[0].done && stats[0].elapsed_ms == 130 && stats[0].rows == 2 &&
                  stats[1].done && stats[1].rows == 1,
              "a handler still running keeps what it gave last time");
        Check(app::MergeCtxHandlerStats(stats, { Handler(L"{B}", true, 700, 1) }) && stats.size() == 1,
              "a handler no longer queried is forgotten");
    }
    {
        // C4b in the controller: partial answers with handler states.
        app::ContextMenuController controller;
        uint32_t token = 110;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++token; };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.dwg" }, false, L".dwg", false, {}, {});
        controller.CompleteComQuery(token, { Row(3, L"{A}", L"", L"Zip a.zip"), Row(9, L"{B}", L"scan", L"Scan") },
                                    100, false,
                                    { Handler(L"{A}", true, 30, 1), Handler(L"{B}", true, 400, 1) });
        controller.TakeComLayoutDirty();
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\b.dwg" }, false, L".dwg", false, {}, {});
        const auto partial = controller.CompleteComQuery(
            token, { Row(30, L"{A}", L"", L"Zip b.zip"), Row(31, L"{A}", L"", L"Unzip b.zip") }, 150, true,
            { Handler(L"{A}", true, 25, 2), Handler(L"{B}", false, 80) });
        const auto& shown = controller.com_items();
        Check(partial.accepted && Texts(shown) == L"Zip b.zip|Unzip b.zip|Scan|" && shown[1].id == 31 &&
                  Predicted(shown[2].id),
              "a partial answer shows the finished handler's new rows and keeps the running one's");
        auto snapshot = controller.ComLayoutSnapshot();
        Check(controller.TakeComLayoutDirty() && snapshot.at(L".dwg").rows.size() == 3,
              "a partial answer with handler states updates the stored layout");
        controller.CompleteComQuery(token, { Row(30, L"{A}", L"", L"Zip b.zip"),
                                             Row(31, L"{A}", L"", L"Unzip b.zip") }, 5100, false,
                                    { Handler(L"{A}", true, 25, 2), Handler(L"{B}", false, 5000) });
        snapshot = controller.ComLayoutSnapshot();
        const auto* stats = controller.ComHandlerStats(L".dwg");
        Check(controller.com_items().size() == 2 && snapshot.at(L".dwg").rows.size() == 3 &&
                  snapshot.at(L".dwg").rows[2].text == L"Scan",
              "a stuck handler leaves the menu on screen but keeps its rows for the next one");
        Check(stats && stats->size() == 2 && (*stats)[1].done && (*stats)[1].elapsed_ms == 400 &&
                  (*stats)[0].elapsed_ms == 25,
              "per-handler times are kept with the layout");
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\c.dwg" }, false, L".dwg", false, {}, {});
        Check(Texts(controller.com_items()) == L"Zip c.zip|Unzip c.zip|Scan|",
              "the next menu paints every handler's last rows with its own name");
        controller.CompleteComQuery(token, { Row(41, L"{A}", L"", L"Zip c.zip") }, 160, true);
        Check(Texts(controller.com_items()) == L"Zip c.zip|Unzip c.zip|Scan|",
              "an answer without handler states keeps the old overlay rules");
    }
    {
        // ctx_layout.bin: bytes, build stamp, fingerprints, abandoned writes.
        app::CtxLayoutRecords records;
        records[L".dwg"] = { 11, { Row(4, L"{A}", L"", (std::wstring(L"to ") + app::kCtxNameStem).c_str()) } };
        records[L".dwg"].rows[0].mnemonic = L'Z';
        records[L".dwg"].rows[0].child = true;
        records[L".dwg"].handlers = { { L"{A}", 75, 2, true }, { L"{S}", 5000, 0, false } };
        records[L":bg"] = { 12, {} };
        const auto bytes = app::SerializeCtxLayout(records, L"1.0+test");
        app::CtxLayoutRecords parsed;
        Check(app::ParseCtxLayout(bytes.data(), bytes.size(), L"1.0+test", parsed) &&
                  parsed.size() == 2 && parsed[L".dwg"].fingerprint == 11 &&
                  parsed[L".dwg"].rows[0].text == records[L".dwg"].rows[0].text &&
                  parsed[L".dwg"].rows[0].mnemonic == L'Z' && parsed[L".dwg"].rows[0].child &&
                  parsed[L".dwg"].handlers.size() == 2 && parsed[L".dwg"].handlers[0].rows == 2 &&
                  parsed[L".dwg"].handlers[1].clsid == L"{S}" && !parsed[L".dwg"].handlers[1].done &&
                  parsed[L".dwg"].handlers[1].elapsed_ms == 5000,
              "the layout survives a round trip with its handler times");
        Check(!app::ParseCtxLayout(bytes.data(), bytes.size(), L"1.1+other", parsed) &&
                  parsed.empty(),
              "a file written by another build is ignored");
        Check(!app::ParseCtxLayout(bytes.data(), bytes.size() - 3, L"1.0+test", parsed),
              "a truncated file is rejected");

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L'\\') + 1) + L"ctx_layout_test.bin";
        DeleteFileW(path.c_str());
        app::CtxLayoutMap layout;
        layout[L".dwg"] = { { Row(4, L"{A}", L"", L"one") }, { { L"{A}", 140, 1, true } } };
        layout[L".dwg|m"] = { { Row(5, L"{A}", L"", L"two") } };
        layout[L":bg"] = { { Row(6, L"{B}", L"", L"three") } };
        auto written = [](const std::wstring& key) -> uint64_t { return key == L":bg" ? 2 : 1; };
        auto changed = [](const std::wstring& key) -> uint64_t { return key == L":bg" ? 3 : 1; };
        Check(app::SaveCtxLayoutFile(path, L"b", layout, written), "the layout is written");
        app::CtxLayoutMap back;
        Check(app::LoadCtxLayoutFile(path, L"b", written, back) && back.size() == 3 &&
                  back[L".dwg"].handlers.size() == 1 && back[L".dwg"].handlers[0].elapsed_ms == 140 &&
                  back[L".dwg"].handlers[0].done,
              "an unchanged registry keeps every key with its handler times");
        Check(app::LoadCtxLayoutFile(path, L"b", changed, back) && back.size() == 2 &&
                  back.contains(L".dwg") && back.contains(L".dwg|m") && !back.contains(L":bg"),
              "a changed fingerprint drops only its keys (both selection sizes)");
        app::CtxLayoutMap newer;
        newer[L".txt"] = { { Row(7, L"{C}", L"", L"four") } };
        Check(!app::SaveCtxLayoutFile(path, L"b", newer, written, [] { return false; }) &&
                  app::LoadCtxLayoutFile(path, L"b", written, back) && back.size() == 3,
              "an abandoned write leaves the file as it was");
        DeleteFileW(path.c_str());
        Check(!app::LoadCtxLayoutFile(path, L"b", written, back) && back.empty(),
              "no file, no layout");

        const uint64_t txt = app::CtxLayoutFingerprint(L".txt");
        Check(txt != 0 && txt == app::CtxLayoutFingerprint(L".txt") &&
                  txt != app::CtxLayoutFingerprint(L".dwg") &&
                  app::CtxLayoutFingerprint(L":bg") == app::CtxLayoutFingerprint(L":bg"),
              "registry fingerprints are stable and per type");
        Check(app::CtxLayoutBaseKey(L".dwg|m") == L".dwg" && app::CtxLayoutBaseKey(L":bg") == L":bg",
              "selection size does not change the fingerprinted type");
    }
    {
        // The shown rows keep their places; late rows are appended, flat.
        using app::CmdShellComBase;
        const int predicted = static_cast<int>(CmdShellComBase + 0x7F00);
        std::vector<ui::FluentMenuItem> shown_rows = {
            MenuRow(1, L"Open"), MenuRow(2, L"Copy", true), MenuRow(predicted, L"Scan"),
            MenuFlyout(L"Zip", { predicted + 1 }), MenuRow(3, L"Manage") };
        shown_rows[3].separator_after = true;
        ui::FluentMenuItem late_flyout = MenuFlyout(L"Git", { 8100, 8101 });
        const std::vector<ui::FluentMenuItem> fresh = {
            MenuRow(1, L"Open"), MenuRow(2, L"Copy", true),
            MenuFlyout(L"zip", { 8020, 8021 }), MenuRow(8040, L"Late"), late_flyout,
            MenuRow(3, L"Manage") };
        app::CtxReconcileStats stats;
        const auto out = app::ReconcileCtxDisplay(shown_rows, fresh, &stats);
        bool kept = out.size() == shown_rows.size() + 2;
        for (size_t i = 0; kept && i + 1 < shown_rows.size(); ++i)
            kept = out[i].separator_after == shown_rows[i].separator_after;
        Check(kept, "a reconciled menu keeps every shown row and separator");
        Check(out[2].text == L"Scan" && !out[2].enabled && out[2].command == predicted,
              "a predicted row the answer lacks stays in place, greyed");
        Check(out[3].children.size() == 2 && out[3].children[0].command == 8020 && out[3].enabled,
              "a matched row takes the live flyout (labels match case-insensitively)");
        Check(out[4].command == 3 && out[4].separator_after && out[0].command == 1,
              "built-in rows stay where they were; the last gains a separator");
        Check(out[5].command == 8040 && out[5].enabled && out[6].text == L"Git" &&
                  out[6].children.size() == 2 && out[6].children[1].command == 8101 &&
                  !out[6].separator_after,
              "late rows are appended flat below, a late flyout stays its own flyout");
        Check(stats.late == 2 && stats.greyed == 1,
              "the reconciliation counts late and greyed rows");

        const std::vector<ui::FluentMenuItem> matched = {
            MenuRow(1, L"Open"), MenuRow(2, L"Copy", true), MenuRow(8031, L"Scan"),
            MenuFlyout(L"Zip", { 8020 }), MenuRow(3, L"Manage") };
        auto settled = app::ReconcileCtxDisplay(shown_rows, matched, &stats);
        Check(SameShape(settled, shown_rows) && stats.late == 0 && stats.greyed == 0 &&
                  settled[2].command == 8031 && settled[2].enabled,
              "nothing late: the menu keeps its exact shape, predicted rows go live");
        // Rows appended once are shown rows: the next answer matches them.
        auto again = app::ReconcileCtxDisplay(out, fresh, &stats);
        Check(SameShape(again, out) && stats.late == 0,
              "rows appended once are not appended again");

        // Any partial order of arrivals: every subset of the shell rows.
        const std::vector<ui::FluentMenuItem> pool = {
            MenuRow(8031, L"Scan"), MenuFlyout(L"Zip", { 8020 }), MenuRow(8040, L"Late"),
            late_flyout, MenuRow(8050, L"Send") };
        bool prefix = true;
        bool counted = true;
        for (unsigned mask = 0; mask < (1u << pool.size()); ++mask) {
            std::vector<ui::FluentMenuItem> part = { MenuRow(1, L"Open"), MenuRow(2, L"Copy", true) };
            uint32_t unpredicted = 0;
            for (size_t k = 0; k < pool.size(); ++k) {
                if (!(mask & (1u << k))) continue;
                part.push_back(pool[k]);
                if (k >= 2) ++unpredicted;
            }
            part.back().separator_after = true;
            part.push_back(MenuRow(3, L"Manage"));
            auto step = app::ReconcileCtxDisplay(shown_rows, part, &stats);
            for (size_t i = 0; i < shown_rows.size(); ++i)
                prefix &= step[i].text == shown_rows[i].text &&
                          (i + 1 == shown_rows.size() || step[i].separator_after == shown_rows[i].separator_after);
            counted &= stats.late == unpredicted && step.size() == shown_rows.size() + unpredicted;
        }
        Check(prefix, "under every partial answer the shown rows stay first, in place");
        Check(counted, "exactly the rows the first frame did not predict are appended");
    }

    {
        // A cold type: rows arrive below the first frame, then sit in place.
        uint32_t token = 40;
        uint32_t invoked = 0;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++token; };
        operations.invoke = [&](uint32_t, uint32_t id, std::wstring, std::wstring) { invoked = id; };
        app::ContextMenuController controller;
        controller.SetShellOperations(std::move(operations));
        const std::vector<ui::FluentMenuItem> base = { MenuRow(1, L"Open"), MenuRow(2, L"Copy") };
        bool changed = false;

        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.psd" }, false, L".psd", false, {}, {});
        controller.OpenMenu(base);
        const auto cold = controller.BuildOpenDisplay(prefs, controller.base_items(), changed);
        controller.BeginTiming(3, cold.size(), false, 1000);
        bool no_more_row = true;
        for (const auto& row : cold) no_more_row &= row.children.empty() || row.enabled;
        Check(controller.MayGrowAfterOpen() && controller.may_grow_at_open() && cold.size() == 3 &&
                  no_more_row,
              "a type with no stored layout opens without any 更多 row");
        controller.CompleteComQuery(token, { Row(70, L"{P}", L"", L"Photoshop"),
                                             Row(71, L"{Q}", L"", L"Scan") },
                                    1100, false, { Handler(L"{P}", true, 40, 1),
                                                   Handler(L"{Q}", true, 90, 1) });
        Check(controller.AnswersComplete(), "the complete answer settles the menu");
        auto update = controller.ReconcileDisplay(prefs, changed, 1100);
        Check(update.size() == cold.size() + 2 && update[0].text == cold[0].text &&
                  update[2].text == cold[2].text && update[2].separator_after &&
                  update[3].text == L"Photoshop" && update[4].text == L"Scan",
              "rows of a cold type are appended below the first frame");
        controller.CommitDisplay(update);
        controller.NoteDisplayUpdate(update.size(), 1100);
        Check(controller.reconcile_stats().late == 2, "the committed late rows add up");
        auto repeat = controller.ReconcileDisplay(prefs, changed, 1100);
        controller.CommitDisplay(repeat);
        Check(repeat.size() == update.size() && controller.reconcile_stats().late == 2,
              "a later answer with the same rows appends nothing");
        controller.ExecuteShellCommand(update[4].command, {}, {});
        Check(invoked == 71, "an appended row runs its live command");
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(true, timing) && timing.may_grow && timing.late == 2 &&
                  timing.grew == 1 && timing.dropped == 0,
              "the timing record says the menu grew once, by the late rows");

        controller.StartQuery(prefs, nullptr, { L"C:\\f\\b.psd" }, false, L".psd", false, {}, {});
        controller.OpenMenu(base);
        const auto warm = controller.BuildOpenDisplay(prefs, controller.base_items(), changed);
        Check(controller.com_predicted() && !controller.MayGrowAfterOpen() &&
                  RowIndex(warm, L"Photoshop") == 2 && RowIndex(warm, L"Scan") == 3,
              "the next menu of the type has the rows in place");
        controller.CompleteComQuery(token, { Row(80, L"{P}", L"", L"Photoshop") }, 1200, false,
                                    { Handler(L"{P}", true, 40, 1), Handler(L"{Q}", true, 90, 0) });
        auto gone = controller.ReconcileDisplay(prefs, changed, 1100);
        Check(SameShape(gone, warm) && gone[2].command == static_cast<int>(app::CmdShellComBase + 80) &&
                  !gone[3].enabled && gone[3].text == L"Scan",
              "a stored row this selection lacks is greyed, not removed");
        controller.CloseMenu();

        // Partial answers of a cold type: one growth step, not one per answer.
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.cdr" }, false, L".cdr", false, {}, {});
        controller.OpenMenu(base);
        const auto cdr = controller.BuildOpenDisplay(prefs, controller.base_items(), changed);
        controller.BeginTiming(1, cdr.size(), false, 3000);
        controller.CompleteComQuery(token, { Row(60, L"{P}", L"", L"CorelDRAW") }, 3050, true,
                                    { Handler(L"{P}", true, 40, 1), Handler(L"{Q}", false, 50) });
        auto held = controller.ReconcileDisplay(prefs, changed, 3050);
        controller.CommitDisplay(held);
        controller.NoteDisplayUpdate(held.size(), 3050);
        controller.CompleteComQuery(token, { Row(60, L"{P}", L"", L"CorelDRAW"),
                                             Row(61, L"{Q}", L"", L"Trace") }, 3200, true,
                                    { Handler(L"{P}", true, 40, 1), Handler(L"{Q}", true, 150, 1),
                                      Handler(L"{R}", false, 150) });
        auto still_held = controller.ReconcileDisplay(prefs, changed, 3200);
        controller.CommitDisplay(still_held);
        controller.NoteDisplayUpdate(still_held.size(), 3200);
        Check(!controller.AnswersComplete() && held.size() == cdr.size() &&
                  still_held.size() == cdr.size(),
              "late rows of partial answers wait instead of growing the menu each time");
        auto batch = controller.ReconcileDisplay(prefs, changed, 3050 + app::ContextMenuController::kLateBatchMs);
        controller.CommitDisplay(batch);
        controller.NoteDisplayUpdate(batch.size(), 3400);
        Check(batch.size() == cdr.size() + 2 && RowIndex(batch, L"CorelDRAW") >= static_cast<int>(cdr.size()) &&
                  RowIndex(batch, L"Trace") >= static_cast<int>(cdr.size()),
              "after the batch time every late row arrives in one step");
        controller.CloseMenu();
        Check(controller.TakeTiming(false, timing) && timing.late == 2 && timing.grew == 1 &&
                  timing.dropped == 0,
              "a cold menu with several partial answers grows once");

        // An answer the open menu never took: its late rows count as dropped.
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.ai" }, false, L".ai", false, {}, {});
        controller.OpenMenu(base);
        const auto ai = controller.BuildOpenDisplay(prefs, controller.base_items(), changed);
        controller.BeginTiming(1, ai.size(), false, 2000);
        controller.CompleteComQuery(token, { Row(90, L"{P}", L"", L"Illustrator") }, 2100, false,
                                    { Handler(L"{P}", true, 40, 1) });
        controller.ReconcileDisplay(prefs, changed, 1100);  // refused by the menu: not committed
        controller.CloseMenu();
        Check(controller.TakeTiming(false, timing) && timing.late == 0 && timing.dropped == 1 &&
                  timing.grew == 0,
              "late rows of an answer the menu refused are reported as dropped");
    }
    {
        // 以管理员身份运行 on a .bat: batfile's runas command (cmd.exe /C "%1")
        // started as a command line runs unelevated. The verb goes to the shell.
        Check(app::ShellOwnsStaticVerb(L"runas") && app::ShellOwnsStaticVerb(L"RunAs") &&
                  app::ShellOwnsStaticVerb(L"runasuser") && !app::ShellOwnsStaticVerb(L"edit") &&
                  !app::ShellOwnsStaticVerb(L"runa") && !app::ShellOwnsStaticVerb(L""),
              "runas / runasuser (any case) are left to the shell");
        struct Case { const wchar_t* verb; bool shell; };
        for (const Case c : { Case{ L"runas", true }, Case{ L"edit", false } }) {
            app::ContextMenuController controller;
            app::ContextMenuPrefs bat_prefs; bat_prefs.persist = false;
            std::wstring by_verb, by_command;
            app::ContextMenuController::ShellOperations operations;
            operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
            operations.execute_verb = [&](const auto&, const auto& verb) { by_verb = verb; };
            operations.execute_command = [&](const auto& command, const auto&) { by_command = command; };
            controller.SetShellOperations(std::move(operations));
            app::StaticVerb row; row.verb = c.verb; row.display = c.verb;
            row.command = L"C:\\Windows\\System32\\cmd.exe /C \"%1\" %*";
            controller.CompleteStaticVerbs(L".bat", { row }, controller.cache_generation());
            controller.StartQuery(bat_prefs, nullptr, { L"C:\\fixture\\setup.bat" }, false, L".bat", false, {}, {});
            controller.OpenMenu({});
            controller.CloseMenu();
            controller.ExecuteShellCommand(app::CmdShellStaticBase, {}, {});
            if (c.shell)
                Check(by_verb == L"runas" && by_command.empty(),
                      "runas with a registry command is invoked as the shell verb (elevates)");
            else
                Check(by_verb.empty() && by_command == row.command,
                      "other static verbs keep their registry command line");
        }
    }
    {
        app::ContextMenuPrefs filter_prefs;
        filter_prefs.persist = false;
        app::ShellMenuEntry hidden;
        hidden.text = L"诊断隐藏项";
        hidden.command = 12001;
        hidden.from_com = false;
        filter_prefs.SetItemEnabled(ipc::CatalogKey(hidden.text, false), false);
        app::ShellMenuEntry duplicate;
        duplicate.text = L"诊断重复项";
        duplicate.command = 12002;
        duplicate.from_com = false;
        const auto filtered = app::ApplyExplorerPrefs(filter_prefs, { hidden, duplicate, duplicate });
        Check(filtered.size() == 2 && filtered[0].text == duplicate.text &&
              filtered[1].text == duplicate.text,
              "an explicit preference hides the Chinese label before append");
        std::vector<ui::FluentMenuItem> display;
        app::AppendShellSection(display, filtered);
        Check(display.size() == 1 && display[0].text == duplicate.text &&
              display[0].command == duplicate.command,
              "duplicate Chinese labels retain exactly one actionable row");
    }

    {
        app::ContextMenuPrefs catalog_prefs;
        const auto category = ipc::CtxMenuCategory::Software;
        for (int i = 0; i < 400; ++i) {
            const auto key = L"v:catalog-" + std::to_wstring(i);
            catalog_prefs.RecordSeen(key, key.substr(2), false, category);
        }
        catalog_prefs.SetItemEnabled(L"v:catalog-0", false);
        catalog_prefs.RecordSeen(L"v:catalog-1", L"catalog-1", false, category);
        Check(catalog_prefs.RecordSeen(L"v:new-visible", L"New visible", false, category) && catalog_prefs.seen.size() == 400,
              "saturated settings catalog admits newly visible Shell row within bound");
        const auto has = [&](const std::wstring& key) {
            for (const auto& row : catalog_prefs.seen) if (row.key == key) return true;
            return false;
        };
        Check(has(L"v:catalog-0") && has(L"v:catalog-1") && !has(L"v:catalog-2") && has(L"v:new-visible"),
              "catalog retirement preserves configured and recently encountered rows first");
        for (const auto& row : catalog_prefs.seen) catalog_prefs.SetItemEnabled(row.key, false);
        catalog_prefs.RecordSeen(L"v:after-configured", L"After configured", false, category);
        Check(catalog_prefs.seen.size() == 400 && has(L"v:after-configured") &&
              catalog_prefs.item_enabled.contains(L"v:catalog-0") && !catalog_prefs.item_enabled.at(L"v:catalog-0"),
              "all-configured catalog still admits new rows without losing evicted override");
        catalog_prefs.RecordSeen(L"v:catalog-0", L"catalog-0", false, category);
        Check(has(L"v:catalog-0") && !catalog_prefs.RowEnabled(L"v:catalog-0", category, false),
              "rediscovered retired row keeps the user's hidden preference");
        app::ContextMenuPrefs loaded;
        Check(loaded.FromJson(catalog_prefs.ToJson()) && loaded.seen.size() <= 400 &&
              !loaded.RowEnabled(L"v:catalog-0", category, false),
              "bounded discovery catalog and retained overrides survive persistence");
        for (int i = 0; i < 50; ++i) {
            auto extra = catalog_prefs.seen.back();
            extra.text = L"overflow-" + std::to_wstring(i); extra.key = L"v:" + extra.text;
            catalog_prefs.seen.push_back(std::move(extra));
        }
        Check(loaded.FromJson(catalog_prefs.ToJson()) && loaded.seen.size() == 400 &&
              !loaded.RowEnabled(L"v:catalog-0", category, false),
              "loading oversized legacy catalog enforces bound without dropping overrides");
    }

    {
        // Native command, pin and package tokens are larger than Shell IDs.
        // They must reach their own dispatchers, not be consumed as COM verbs.
        for (int command : {app::CmdShellComLast + 1, 0x20000000, 0x2FFFFFFF,
                            0x30000000, 0x40000000}) {
            app::ContextMenuController controller;
            int invoked = 0, waited = 0, refreshed = 0;
            app::ContextMenuController::ShellOperations operations;
            operations.invoke = [&](uint32_t, uint32_t, auto, auto) { ++invoked; };
            controller.SetShellOperations(std::move(operations));
            const bool handled = controller.ExecuteShellCommand(command,
                [&] { ++waited; }, [&] { ++refreshed; });
            Check(!handled && invoked == 0 && waited == 0 && refreshed == 0,
                  "non-Shell command falls through without invoking or waiting for Explorer");
        }
        app::ContextMenuController controller;
        int refreshed = 0;
        Check(controller.ExecuteShellCommand(app::CmdShellComLast, {}, [&] { ++refreshed; }) && refreshed == 1,
              "last reserved Shell ID remains handled, including stale tokens");
    }

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
