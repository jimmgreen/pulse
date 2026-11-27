#include "../app/places.h"
#include "../fs/fs_enum.h"
#include "../app/context_menu_prefs.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <cstdio>
#include <functional>
#include <string_view>
#include <chrono>
#include <tuple>
#include <algorithm>
#include <iterator>

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
    static void Index(PlacesCatalog& c) { c.RebuildTagIndex(); c.RebuildStarIndex(); }
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
static void SeedCopyCatalog(PlacesCatalog& c) {
    c.persist = false;
    c.tags.clear(); c.starred_items.clear(); c.starred_groups.clear(); c.recent_items.clear();
    ColorTag tag; tag.id = L"copy-tag"; tag.name = L"copy"; tag.rgb = 0x123456; tag.favorite = 1; tag.group = L"tags";
    const wchar_t* paths[] = {L"C:\\src\\a.jpg", L"C:\\src\\b.jpg", L"C:\\src\\dir\\c.jpg", L"C:\\src\\dir",
        L"C:\\keep\\d.jpg", L"C:\\src\\directory\\e.jpg", L"\\\\server\\share\\src\\item.jpg", L"C:\\src\\a.jpg"};
    for (size_t i = 0; i < std::size(paths); ++i) {
        const auto path = pulse::fs::NormalizePath(paths[i]);
        tag.paths.push_back(path);
        if (i + 1 == std::size(paths)) continue;
        StarredItem star; star.path = path; star.kind = i == 3 ? PlaceItemKind::Folder : PlaceItemKind::File;
        star.badge = std::to_wstring(i); star.badge_rgb = 0x135790 + static_cast<uint32_t>(i);
        star.group = i == 3 ? L"favorites" : L""; star.separator_after = true;
        c.starred_items.push_back(star);
        RecentItem recent; recent.path = path; recent.kind = star.kind; recent.opened_at = 42 + i;
        c.recent_items.push_back(recent);
    }
    c.tags.push_back(tag);
    ColorTag second; second.id = L"second"; second.name = L"second"; second.paths = {tag.paths[2], tag.paths[4]};
    c.tags.push_back(second);
    StarredGroup group; group.id = L"favorites"; group.name = L"favorites"; group.collapsed = true;
    c.starred_groups.push_back(group);
    c.quick_access_paths = {pulse::fs::NormalizePath(L"C:\\src\\dir")};
    c.quick_access_order = {L"builtin:recent", c.quick_access_paths.front()};
    PlacesPersistenceAudit::Index(c);
}
static bool SameCopyCatalog(const PlacesCatalog& a, const PlacesCatalog& b) {
    if (a.tags.size() != b.tags.size() || a.starred_items.size() != b.starred_items.size() ||
        a.recent_items.size() != b.recent_items.size() || a.quick_access_paths != b.quick_access_paths ||
        a.quick_access_order != b.quick_access_order || a.starred_groups.size() != b.starred_groups.size()) return false;
    for (size_t i = 0; i < a.tags.size(); ++i) {
        const auto& x = a.tags[i]; const auto& y = b.tags[i];
        if (std::tie(x.id,x.name,x.rgb,x.favorite,x.group,x.paths) != std::tie(y.id,y.name,y.rgb,y.favorite,y.group,y.paths)) return false;
        for (const auto& path : x.paths) if (a.TagsForPath(path) != b.TagsForPath(path)) return false;
    }
    for (size_t i = 0; i < a.starred_items.size(); ++i) {
        const auto& x = a.starred_items[i]; const auto& y = b.starred_items[i];
        if (std::tie(x.path,x.kind,x.badge,x.badge_rgb,x.group,x.separator_after) !=
            std::tie(y.path,y.kind,y.badge,y.badge_rgb,y.group,y.separator_after) || a.IsStarred(x.path) != b.IsStarred(x.path)) return false;
    }
    for (size_t i = 0; i < a.recent_items.size(); ++i) {
        const auto& x = a.recent_items[i]; const auto& y = b.recent_items[i];
        if (std::tie(x.path,x.kind,x.opened_at) != std::tie(y.path,y.kind,y.opened_at)) return false;
    }
    for (size_t i = 0; i < a.starred_groups.size(); ++i) {
        const auto& x = a.starred_groups[i]; const auto& y = b.starred_groups[i];
        if (std::tie(x.id,x.name,x.collapsed,x.separator_after) != std::tie(y.id,y.name,y.collapsed,y.separator_after)) return false;
    }
    return true;
}
static std::vector<std::wstring> ReferenceTaggedPaths(const PlacesCatalog& c, const std::vector<std::wstring>& roots) {
    std::vector<std::wstring> result;
    for (const auto& tag : c.tags) for (const auto& path : tag.paths) {
        const bool included = std::any_of(roots.begin(), roots.end(), [&](const auto& root) {
            if (_wcsicmp(path.c_str(), root.c_str()) == 0) return true;
            return path.size() > root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
                (root.ends_with(L"\\") || path[root.size()] == L'\\' || path[root.size()] == L'/');
        });
        if (included) result.push_back(path);
    }
    return result;
}
static void CopyCompletionCases() {
    using Copies = std::vector<std::pair<std::wstring, std::wstring>>;
    const std::vector<Copies> cases = {
        {{L"C:\\src\\b.jpg",L"C:\\dst\\b.jpg"},{L"c:\\SRC\\a.jpg",L"C:\\dst\\a.jpg"},{L"C:\\src\\dir",L"C:\\dst\\dir"}},
        {{L"C:\\src\\a.jpg",L"C:\\src\\b.jpg"},{L"C:\\src\\b.jpg",L"C:\\other\\b.jpg"}},
        {{L"C:\\src",L"C:\\copied"},{L"C:\\src\\dir",L"C:\\other\\dir"}},
        {{L"C:\\src\\a.jpg",L"C:\\same.jpg"},{L"C:\\src\\a.jpg",L"C:\\same.jpg"}},
        {{L"C:\\src\\a.jpg",L"C:\\same.jpg"},{L"C:\\src\\b.jpg",L"C:\\same.jpg"}},
        {{L"C:\\src\\a.jpg",L"C:\\src\\a.jpg"},{L"C:\\src\\a.jpg",L"c:\\SRC\\A.JPG"}},
        {{L"",L"C:\\missing"},{L"C:\\not-present",L"C:\\missing"}},
        {{L"\\\\server\\share\\src",L"\\\\server\\share\\dst"}},
        {{L"C:\\",L"D:\\copy"}},
        {{L"C:\\src\\dir",L"C:\\other\\dir"},{L"C:\\other",L"C:\\second"}}
    };
    for (const auto& copies : cases) {
        PlacesCatalog batch, single; SeedCopyCatalog(batch); SeedCopyCatalog(single);
        batch.CloneAssignments(copies);
        for (const auto& [from,to] : copies) single.CloneAssignments(from,to);
        Check(SameCopyCatalog(batch,single), "copy batch preserves sequential metadata, order, duplicate handling and lookup indexes");
    }
    PlacesCatalog c; SeedCopyCatalog(c);
    c.CloneAssignments(Copies{{L"C:\\src\\dir",L"C:\\dst\\dir"}});
    const auto copied_star = c.FindStarred(L"C:\\dst\\dir");
    Check(copied_star && c.IsStarred(L"C:\\src\\dir") && c.IsStarred(L"C:\\dst\\dir") &&
        copied_star->group == L"favorites" && !copied_star->separator_after,
        "copy keeps source and favorite group while clearing only the new separator flag");
    Check(!c.TagsForPath(L"C:\\dst\\dir\\c.jpg").empty() && c.TagsForPath(L"C:\\dst\\directory\\e.jpg").empty(),
        "copy includes descendants but not a similar directory-name prefix");
    const auto N = [](const wchar_t* path) { return pulse::fs::NormalizePath(path); };
    for (const auto& roots : std::vector<std::vector<std::wstring>>{
            {}, {N(L"C:\\src\\dir")}, {N(L"C:\\src"),N(L"C:\\src\\dir")},
            {N(L"c:\\SRC\\A.JPG"),N(L"C:\\src\\a.jpg")}, {N(L"\\\\server\\share\\src")},
            {N(L"C:\\")}, {N(L"C:\\missing")}, {L""}, {L"C:/src/"}, {L"pulse:tags"}}) {
        Check(c.TaggedPathsUnder(roots) == ReferenceTaggedPaths(c,roots),
            "tag descendant expansion preserves catalog order, duplicates, case and path-boundary semantics");
    }
}
static void CompletionBenchmark() {
    using Clock = std::chrono::steady_clock;
    for (size_t count : {64u,256u,512u}) for (int sample = 0; sample < 3; ++sample) {
        PlacesCatalog c; c.persist = false;
        c.tags.clear(); c.starred_items.clear();
        ColorTag tag; tag.id = L"bench"; tag.name = L"bench";
        for (size_t i = 0; i < count * 12; ++i) {
            const auto raw = i % 12 == 0 ? L"C:\\completion-bench\\src\\f" + std::to_wstring(i/12) :
                L"C:\\completion-bench\\unrelated\\f" + std::to_wstring(i);
            tag.paths.push_back(pulse::fs::NormalizePath(raw + L".bin"));
            if (i % 48 == 0) { StarredItem star; star.path = tag.paths.back(); star.kind = PlaceItemKind::File; c.starred_items.push_back(star); }
        }
        c.tags.push_back(std::move(tag)); PlacesPersistenceAudit::Index(c);
        std::vector<std::pair<std::wstring,std::wstring>> copies;
        std::vector<std::wstring> roots;
        for (size_t i = count; i-- > 0;) {
            const auto from = pulse::fs::NormalizePath(L"C:\\completion-bench\\src\\f"+std::to_wstring(i)+L".bin");
            const auto to = pulse::fs::NormalizePath(L"C:\\completion-bench\\dst\\f"+std::to_wstring(i)+L".bin");
            copies.emplace_back(from,to); roots.push_back(to);
        }
        const auto start = Clock::now(); c.CloneAssignments(copies); const auto copied = Clock::now();
        const auto matches = c.TaggedPathsUnder(roots); const auto matched = Clock::now();
        const auto first = pulse::fs::NormalizePath(L"C:\\completion-bench\\dst\\f0.bin");
        Check(c.tags[0].paths.size() == count*13 && c.starred_items.size() == count/2 &&
            matches.size() == count && !c.TagsForPath(first).empty() && c.IsStarred(first), "benchmark verifies production metadata output without disk writes");
        printf("COMPLETION_BENCH roots=%zu assigned=%zu sample=%d copy_ms=%.3f match_ms=%.3f\n", count,
            count*13,sample,std::chrono::duration<double,std::milli>(copied-start).count(),
            std::chrono::duration<double,std::milli>(matched-copied).count()); fflush(stdout);
    }
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--completion-only") {
        CopyCompletionCases(); printf("COPY_METADATA_RESULT failures=%d\n",failures); return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--completion-bench") {
        CompletionBenchmark(); return failures ? 1 : 0;
    }
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
