#include "../app/places.h"
#include "../fs/fs_enum.h"
#include "../app/context_menu_prefs.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <cstdio>
#include <functional>
#include <string_view>

namespace pulse::app {
static std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
struct PlacesPersistenceAudit {
    static std::mutex& Io(PlacesCatalog& c) { return c.places_save_io_mutex_; }
    static uint64_t Revision(PlacesCatalog& c) {
        std::lock_guard lock(c.places_save_mutex_);
        return c.places_save_revision_;
    }
    static bool Dirty(PlacesCatalog& c) {
        std::lock_guard lock(c.places_save_mutex_);
        return c.places_save_due_ != 0;
    }
    static void Mark(PlacesCatalog& c) { c.MarkPlacesDirty(); }
    static void Queue(PlacesCatalog& c) { c.QueuePlacesSave(); }
    static void Stop(PlacesCatalog& c) { c.StopPlacesWriter(); }
};
}
using namespace pulse;
using namespace pulse::app;
static int failures;
static void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(stdout); failures += !ok;
}
static bool Wait(const std::function<bool()>& ready) {
    const auto until = GetTickCount64() + 4000;
    while (!ready()) { if (GetTickCount64() >= until) return false; Sleep(10); }
    return true;
}
static std::wstring Read(const wchar_t* name) {
    std::wstring value; ReadUtf8File(fixture + L"\\" + name, value); return value;
}
static void Menu() {
    for (const auto* label : {L"Pin to Quick access", L"固定到快速访问", L"固定到快速存取"}) {
        ContextMenuPrefs c;
        const auto alias = ipc::CatalogKey(label, false);
        c.seen.push_back({alias, label});
        c.item_enabled[alias] = false;
        c.MigrateSeenKeys();
        ContextMenuPrefs reloaded;
        Check(reloaded.FromJson(c.ToJson()) && reloaded.seen.size() == 1 &&
            reloaded.seen.front().key == L"pulse:quick-access" &&
            !reloaded.ItemEnabled(L"pulse:quick-access", ipc::CtxMenuCategory::Software, false),
            "M09-003 legacy translated quick-access choice survives reload");
        c.seen.push_back({L"pulse:quick-access", label});
        c.item_enabled[alias] = true;
        c.item_enabled[L"pulse:quick-access"] = false;
        c.MigrateSeenKeys();
        Check(c.seen.size() == 1 && !c.item_enabled.at(L"pulse:quick-access"),
            "M09-003 stable explicit choice wins and duplicate aliases merge");
    }
    ContextMenuPrefs c;
    c.item_enabled[ipc::CatalogKey(L"固定到快速访问", false)] = false;
    c.item_enabled[ipc::CatalogKey(L"Pin to Quick access", false)] = true;
    c.MigrateSeenKeys();
    Check(!c.item_enabled.at(L"pulse:quick-access"), "M09-003 conflicting legacy aliases deterministically preserve disable");
    c.seen.push_back({L"pulse:future-command", L"translated display"});
    c.seen.push_back({L"h:{fixture}", L"extension display"});
    c.MigrateSeenKeys();
    Check(c.seen[0].key == L"pulse:future-command" && c.seen[1].key == L"h:{fixture}",
        "M09-003 stable application and handler identities remain unchanged");
}
static void WorkspaceRoundTrip() {
    for (size_t empty = 0; empty < 3; ++empty) {
        const std::vector<std::wstring> paths = {
            empty == 0 ? L"" : fixture + L"\\left",
            empty == 1 ? L"" : fixture + L"\\middle",
            empty == 2 ? L"" : fixture + L"\\right"};
        const std::vector<ui::ViewMode> views = {
            ui::ViewMode::Details, ui::ViewMode::Content, ui::ViewMode::List};
        {
            PlacesCatalog writer;
            const int index = writer.PinWorkspace(fixture, L"磁盘恢复测试", 3, paths, views);
            Check(index == 0 && writer.Save(), "M10-004 save workspace with explicit This PC pane to disk");
        }
        {
            PlacesCatalog reader;
            const bool loaded = reader.Load();
            Check(loaded && reader.workspaces.size() == 1 && reader.active_workspace == 0 &&
                reader.workspaces[0].pane_paths == paths && reader.workspaces[0].pane_views == views,
                "M10-004 fresh catalog preserves first/middle/last This PC pane and view alignment");
        }
    }
}
static void QuickAccessLayout() {
    using Keys = std::vector<std::wstring>;
    const Keys defaults = {L"builtin:recent", L"builtin:desktop", L"C:\\a", L"C:\\b"};
    Check(MergeQuickAccessOrder({}, defaults) == defaults, "QA order: no saved order keeps the defaults");
    Check(MergeQuickAccessOrder({L"C:\\b", L"builtin:desktop", L"C:\\A", L"builtin:recent"}, defaults) ==
              Keys{L"C:\\b", L"builtin:desktop", L"C:\\a", L"builtin:recent"},
          "QA order: the saved order wins and paths match case-insensitively");
    const Keys grown = {L"builtin:recent", L"builtin:desktop", L"builtin:pictures",
                        L"C:\\a", L"C:\\b", L"C:\\c"};
    Check(MergeQuickAccessOrder({L"C:\\b", L"builtin:desktop", L"C:\\gone", L"C:\\a", L"builtin:recent"}, grown) ==
              Keys{L"C:\\b", L"builtin:desktop", L"builtin:pictures", L"C:\\a", L"builtin:recent", L"C:\\c"},
          "QA order: a new link follows its default predecessor, a new pin goes last, stale keys drop");

    const Keys keys = {L"builtin:recent", L"builtin:desktop", L"builtin:downloads", L"C:\\a", L"C:\\b"};
    const Keys visible = {L"builtin:recent", L"builtin:desktop", L"C:\\a", L"C:\\b"};
    Check(MoveQuickAccessKey(keys, visible, L"C:\\b", 0) ==
              Keys{L"C:\\b", L"builtin:recent", L"builtin:desktop", L"builtin:downloads", L"C:\\a"},
          "QA move: a pin goes above the built-in links");
    Check(MoveQuickAccessKey(keys, visible, L"builtin:recent", 4) ==
              Keys{L"builtin:desktop", L"builtin:downloads", L"C:\\a", L"C:\\b", L"builtin:recent"},
          "QA move: a link goes to the end");
    Check(MoveQuickAccessKey(keys, visible, L"C:\\a", 2) == keys &&
              MoveQuickAccessKey(keys, visible, L"C:\\a", 3) == keys,
          "QA move: the row's own gaps change nothing");
    Check(MoveQuickAccessKey(keys, visible, L"C:\\a", 1) ==
              Keys{L"builtin:recent", L"C:\\a", L"builtin:desktop", L"builtin:downloads", L"C:\\b"},
          "QA move: hidden links keep their places");

    const Keys builtin = {L"builtin:recent", L"builtin:desktop"};
    // The catalog stores pins in their normalized (extended-length) form.
    const std::wstring a = pulse::fs::NormalizePath(L"C:\\a");
    const std::wstring b = pulse::fs::NormalizePath(L"C:\\b");
    const std::wstring z = pulse::fs::NormalizePath(L"C:\\z");
    PlacesCatalog c;
    c.quick_access_paths = {L"C:\\a", L"C:\\b"};
    Check(c.SetQuickAccessLayout({L"builtin:recent", L"builtin:desktop", L"C:\\b", L"C:\\a"}, builtin) &&
              c.quick_access_order.empty() && c.quick_access_paths == Keys{b, a},
          "QA layout: the default arrangement stores only the pin order");
    Check(c.SetQuickAccessLayout({L"C:\\a", L"builtin:recent", L"builtin:desktop"}, builtin) &&
              c.quick_access_paths == Keys{a} &&
              c.quick_access_order == Keys{a, L"builtin:recent", L"builtin:desktop"},
          "QA layout: a mixed order is stored and dropped pins are unpinned");
    Check(!c.SetQuickAccessLayout({L"C:\\a", L"builtin:recent", L"builtin:desktop"}, builtin),
          "QA layout: an unchanged layout is not saved again");
    {
        PlacesCatalog reader;
        Check(reader.Load() && reader.quick_access_order == c.quick_access_order &&
                  reader.quick_access_paths == c.quick_access_paths,
              "QA layout: the order survives save and reload");
    }
    c.RemapPaths(L"C:\\a", L"C:\\z");
    Check(c.quick_access_order.size() == 3 && c.quick_access_order[0] == z,
          "QA layout: a renamed pinned folder keeps its place");
    c.SetQuickAccessPinned({L"C:\\z"}, false);
    Check(c.quick_access_order == Keys{L"builtin:recent", L"builtin:desktop"},
          "QA layout: unpinning removes the folder from the order");
}
static void BatchedAssignments() {
    // Batch calls must leave what one call per path left.
    const auto N = [](const wchar_t* path) { return pulse::fs::NormalizePath(path); };
    const auto seed = [&](PlacesCatalog& c) {
        c.persist = false;
        c.tags.clear(); c.starred_items.clear(); c.recent_items.clear();
        ColorTag tag; tag.id = L"batch"; tag.name = L"batch";
        for (const wchar_t* path : {L"C:\\src\\a.jpg", L"C:\\src\\b.jpg", L"C:\\src\\dir\\c.jpg",
                                    L"C:\\src\\dir", L"C:\\keep\\d.jpg", L"C:\\src\\ab.jpg"}) {
            tag.paths.push_back(N(path));
            StarredItem star; star.path = N(path); c.starred_items.push_back(star);
            RecentItem recent; recent.path = N(path); c.recent_items.push_back(recent);
        }
        c.tags.push_back(tag);
    };
    const auto same = [](const PlacesCatalog& left, const PlacesCatalog& right) {
        if (left.tags.size() != right.tags.size() || left.starred_items.size() != right.starred_items.size() ||
            left.recent_items.size() != right.recent_items.size()) return false;
        for (size_t i = 0; i < left.tags.size(); ++i)
            if (left.tags[i].paths != right.tags[i].paths) return false;
        for (size_t i = 0; i < left.starred_items.size(); ++i)
            if (left.starred_items[i].path != right.starred_items[i].path) return false;
        for (size_t i = 0; i < left.recent_items.size(); ++i)
            if (left.recent_items[i].path != right.recent_items[i].path) return false;
        return true;
    };
    using Moves = std::vector<std::pair<std::wstring, std::wstring>>;
    for (const Moves& moves : {
             Moves{{L"C:\\src\\a.jpg", L"C:\\dst\\a.jpg"}, {L"c:\\SRC\\DIR", L"C:\\dst\\dir"},
                   {L"C:\\src\\b.jpg", L"C:\\dst\\b.jpg"}},
             // A chain and nested sources fall back to one move after another.
             Moves{{L"C:\\src\\a.jpg", L"C:\\src\\b.jpg"}, {L"C:\\src\\b.jpg", L"C:\\other\\b.jpg"}},
             Moves{{L"C:\\src", L"C:\\moved"}, {L"C:\\src\\dir", L"C:\\elsewhere\\dir"}}}) {
        PlacesCatalog batch, single;
        seed(batch); seed(single);
        batch.RemapPaths(moves);
        for (const auto& [from, to] : moves) single.RemapPaths(from, to);
        Check(same(batch, single), "batched remap leaves what one remap per item left");
    }
    {
        PlacesCatalog batch;
        seed(batch);
        batch.RemapPaths(Moves{{L"C:\\src\\a.jpg", L"C:\\dst\\a.jpg"}, {L"C:\\src\\dir", L"C:\\dst\\dir"}});
        Check(batch.starred_items[0].path == N(L"C:\\dst\\a.jpg") &&
                  batch.starred_items[2].path == N(L"C:\\dst\\dir\\c.jpg") &&
                  batch.starred_items[5].path == N(L"C:\\src\\ab.jpg"),
              "batched remap moves items and descendants but not name prefixes");
    }
    for (bool descendants : {true, false}) {
        const std::vector<std::wstring> removed = {L"c:\\SRC\\A.JPG", L"C:\\src\\dir", L"C:\\missing"};
        PlacesCatalog batch, single;
        seed(batch); seed(single);
        batch.RemoveAssignments(removed, descendants);
        for (const auto& path : removed) single.RemoveAssignments(path, descendants);
        Check(same(batch, single) && batch.starred_items.size() == (descendants ? 3u : 4u),
              "batched removal leaves what one removal per item left");
    }
}
int main(int argc, char** argv) {
    const auto root = std::filesystem::absolute(L"bench_data/places-audit-" + std::to_wstring(GetCurrentProcessId()));
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    fixture = root.wstring();
    if (argc == 2 && std::string_view(argv[1]) == "--save-only") {
        PlacesCatalog c;
        c.quick_access_paths = {L"C:\\first"};
        Check(c.Save(), "initial places save");
        const auto file = root / L"places.json";
        const auto before = std::filesystem::last_write_time(file);
        Sleep(30);
        Check(c.Save() && std::filesystem::last_write_time(file) == before,
              "identical places snapshot does not rewrite the file");
        c.quick_access_paths.push_back(L"C:\\second");
        Check(c.Save() && std::filesystem::last_write_time(file) != before,
              "changed places snapshot is persisted");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--workspace-only") {
        WorkspaceRoundTrip();
        printf("fixtures retained: %ls\n", root.c_str());
        return failures ? 1 : 0;
    }
    Menu();
    QuickAccessLayout();
    BatchedAssignments();
    {
        PlacesCatalog c;
        c.quick_access_paths = {L"C:\\old"};
        PlacesPersistenceAudit::Mark(c);
        std::unique_lock io(PlacesPersistenceAudit::Io(c));
        PlacesPersistenceAudit::Queue(c);
        const auto revision = PlacesPersistenceAudit::Revision(c);
        c.quick_access_paths = {L"C:\\new"};
        bool saved = false;
        std::thread synchronous([&] { saved = c.Save(); });
        Check(Wait([&] { return PlacesPersistenceAudit::Revision(c) > revision; }),
            "M09-001 synchronous save supersedes queued revision before disk access");
        io.unlock(); synchronous.join(); PlacesPersistenceAudit::Stop(c);
        Check(saved && Read(L"places.json").find(L"new") != std::wstring::npos &&
            Read(L"places.json").find(L"old") == std::wstring::npos,
            "M09-001 queued old snapshot cannot overwrite synchronous newer save");
        std::unique_lock next_io(PlacesPersistenceAudit::Io(c));
        PlacesPersistenceAudit::Mark(c); PlacesPersistenceAudit::Queue(c);
        c.quick_access_paths = {L"C:\\latest"}; PlacesPersistenceAudit::Mark(c);
        next_io.unlock(); PlacesPersistenceAudit::Stop(c);
        Check(PlacesPersistenceAudit::Dirty(c) && c.FlushPendingSave(true) &&
            Read(L"places.json").find(L"latest") != std::wstring::npos,
            "M09-001 new dirty revision survives completion of stale writer");
    }
    const auto tags_file = fixture + L"\\tags.json";
    Check(WriteUtf8FileAtomic(tags_file, LR"({"tags":[{"id":"kept","name":"old","rgb":1}]})"), "seed old tags");
    HANDLE lock = CreateFileW(tags_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "lock tags against replacement while allowing recovery load");
    ULONGLONG start = GetTickCount64();
    {
        PlacesCatalog c; Check(c.Load(), "load isolated old tags");
        Check(c.RenameTag(L"kept", L"new pending"), "queue changed tag");
        Check(Wait([&] { return Read(L"tags.pending.json").find(L"new pending") != std::wstring::npos; }),
            "M09-002 failed replace preserves generation-stamped recovery file");
        c.RenameTag(L"kept", L"latest pending");
    }
    Check(GetTickCount64() - start < 5000, "M09-002 permanently locked destination does not hang shutdown");
    CloseHandle(lock);
    {
        PlacesCatalog c;
        Check(c.Load() && c.FindTag(L"kept") && c.FindTag(L"kept")->name == L"latest pending",
            "M09-002 restart recovers latest queued tags rather than stale main file");
        Check(Wait([&] { return Read(L"tags.json").find(L"latest pending") != std::wstring::npos; }),
            "M09-002 recovered tags commit after lock is released");
        lock = CreateFileW(tags_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(lock != INVALID_HANDLE_VALUE && c.RenameTag(L"kept", L"temporary lock"), "queue temporary failure");
        Check(Wait([&] { return Read(L"tags.pending.json").find(L"temporary lock") != std::wstring::npos; }), "observe temporary write failure");
        CloseHandle(lock);
        Check(Wait([&] { return Read(L"tags.json").find(L"temporary lock") != std::wstring::npos; }),
            "M09-002 worker retries without another user edit");
    }
    printf("fixtures retained: %ls\n", root.c_str());
    return failures ? 1 : 0;
}
