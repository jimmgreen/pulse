#include "../app/app_prefs.h"
#include "../app/places.h"
#include "../app/session.h"
#include "../app/app_model.h"
#include "../app/io_task_queue.h"
#include "../app/tag_ads_sync.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <cstdio>
#include <string_view>
#include <deque>
namespace pulse::app { static std::wstring fixture; std::wstring GetPulseDataDir() { return fixture; } }
static int failures;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static int TagAdsOnly() {
    using namespace pulse::app;
    const auto dir = std::filesystem::absolute(L"bench_data/tag-ads-diagnostic-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(dir)) return 2;
    fixture = dir.wstring();
    const auto file = fixture + L"\\private.txt";
    Check(pulse::WriteUtf8FileAtomic(file, L"private"), "create exclusive private fixture");
    Check(WriteTagAdsV2(file, {}), "deleting absent ADS is idempotent");
    Check(WriteTagAdsV2(file, {{L"t", L"label", 0x123456}}), "create private ADS");
    HANDLE held = CreateFileW((file + L":Pulse.Tag").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold private ADS against deletion");
    TagAdsUpdate removal; removal.path = file;
    std::vector<TagAdsFailure> diagnostics;
    Check(!SyncTagAdsUpdates({removal}, &diagnostics).empty(), "locked stream retains pending failure");
    Check(diagnostics.size() == 1 && diagnostics[0].stage == TagAdsFailureStage::StreamWrite &&
        diagnostics[0].error == ERROR_SHARING_VIOLATION && !diagnostics[0].unsupported,
        "sharing violation preserves exact stage and error without unsupported claim");
    if (!diagnostics.empty()) Check(TagAdsFailureMessage(diagnostics[0]).find(L"stream-write") != std::wstring::npos,
        "retry message preserves actionable failure stage");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(SyncTagAdsUpdates({}, &diagnostics).empty() && diagnostics.empty(), "release permits deletion retry");
    Check(WriteTagAdsV2(file, {}) && WriteTagAdsV2(file, {}), "repeated deletion of removed ADS succeeds");
    const auto journal = fixture + L"\\tag_ads_pending.dat";
    held = CreateFileW(journal.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "lock private journal replacement");
    Check(!SyncTagAdsUpdates({removal}, &diagnostics).empty() && HasPendingTagAds(file),
        "journal write failure retains deletion intent");
    Check(diagnostics.size() == 1 && diagnostics[0].stage == TagAdsFailureStage::JournalWrite &&
        diagnostics[0].error != ERROR_SUCCESS && !diagnostics[0].unsupported,
        "journal write failure cannot claim volume unsupported");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(SyncTagAdsUpdates({}, &diagnostics).empty(), "journal recovery retries idempotent deletion");
    const auto old_folder = dir / L"old-folder";
    std::filesystem::create_directory(old_folder);
    const auto old_file = (old_folder / L"moved.txt").wstring();
    Check(pulse::WriteUtf8FileAtomic(old_file, L"original content"), "create moved-file fixture");
    TagAdsUpdate stale; stale.path = old_file;
    stale.tags = {{L"t", L"label", 0x123456}};
    Check(WriteTagAdsV2(old_file, stale.tags), "prepare moved-file tag stream");
    held = CreateFileW((old_file + L":Pulse.Tag").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE && !SyncTagAdsUpdates({stale}, &diagnostics).empty(),
        "locked stream retains pending tag update before move");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    const auto moved_file = dir / L"moved.txt";
    std::filesystem::rename(old_file, moved_file);
    std::filesystem::remove(old_folder);
    Check(SyncTagAdsUpdates({}, &diagnostics).empty() && diagnostics.empty() && !HasPendingTagAds(old_file),
        "retry retires moved file under removed parent on accessible volume");
    Check(!std::filesystem::exists(old_file) && std::filesystem::exists(moved_file),
        "stale retry cannot recreate moved host file");
    stale.path = fixture + L"\\absent.txt";
    Check(SyncTagAdsUpdates({stale}, &diagnostics).empty() && !std::filesystem::exists(stale.path),
        "nonempty tags never recreate missing host in existing directory");
    const auto online_fixture = fixture;
    fixture = (dir / L"offline-profile").wstring(); std::filesystem::create_directory(fixture);
    const DWORD drives = GetLogicalDrives();
    wchar_t unused = L'Z';
    while (unused >= L'D' && (drives & (1u << (unused - L'A')))) --unused;
    Check(unused >= L'D', "find unused drive for offline retry fixture");
    if (unused >= L'D') {
        stale.path = std::wstring(1, unused) + L":\\Pulse-offline-fixture\\tagged.txt";
        Check(!SyncTagAdsUpdates({stale}, &diagnostics).empty() && HasPendingTagAds(stale.path),
            "unavailable drive retains pending intent instead of treating it as deleted");
    }
    fixture = online_fixture;
    std::vector<TagAdsUpdate> batch;
    bool created = true;
    for (int i = 0; i < 65; ++i) {
        wchar_t name[32]{}; swprintf_s(name, L"\\batch-%03d.txt", i);
        TagAdsUpdate update; update.path = fixture + name;
        created = pulse::WriteUtf8FileAtomic(update.path, L"private batch") && created;
        batch.push_back(std::move(update));
    }
    Check(created, "create 65 exclusive private batch files");
    const auto deferred = SyncTagAdsUpdates(batch, &diagnostics);
    Check(deferred.size() == 1 && deferred.front() == batch.back().path &&
        HasPendingTagAds(batch.back().path) && !HasPendingTagAds(batch.front().path),
        "first batch completes 64 and retains exact final pending path");
    Check(diagnostics.size() == 1 && diagnostics[0].stage == TagAdsFailureStage::Pending &&
        diagnostics[0].error == ERROR_IO_PENDING && !diagnostics[0].unsupported,
        "unattempted item explicitly reports pending without fabricating stream failure");
    TagAdsNoticeState mixed_notice;
    std::wstring mixed_title, mixed_message;
    const TagAdsFailure real_failure{file, TagAdsFailureStage::StreamWrite, ERROR_SHARING_VIOLATION};
    auto mixed = diagnostics; mixed.insert(mixed.begin(), real_failure);
    ApplyTagAdsNotice(mixed_notice, mixed, mixed_title, mixed_message, true);
    Check(mixed_message == TagAdsFailureMessage(real_failure), "deferred work cannot hide a real failure diagnostic");
    TagAdsNoticeState batch_notice;
    std::wstring batch_title, batch_message;
    ApplyTagAdsNotice(batch_notice, {{file, TagAdsFailureStage::StreamWrite, ERROR_SHARING_VIOLATION}},
        batch_title, batch_message, true);
    ApplyTagAdsNotice(batch_notice, diagnostics, batch_title, batch_message, false);
    Check(!diagnostics.empty() && !batch_message.empty() && batch_message == TagAdsFailureMessage(diagnostics.front()),
        "first batch cannot clear banner while one item remains pending");
    Check(SyncTagAdsUpdates({}, &diagnostics).empty() && diagnostics.empty() &&
        !HasPendingTagAds(batch.back().path), "second batch completes final item");
    ApplyTagAdsNotice(batch_notice, diagnostics, batch_title, batch_message, false);
    Check(batch_title.empty() && batch_message.empty(), "only fully completed batch clears banner");
    std::wstring journal_text;
    Check(pulse::ReadUtf8File(journal, journal_text) && journal_text == L"PULSE_ADS_PENDING_1\n",
        "final persisted journal contains no pending items");
    std::deque<std::function<void()>> jobs;
    std::vector<TagAdsFailure> last_result;
    auto enqueue = [&](std::function<void()> task) { jobs.push_back(std::move(task)); };
    auto publish = [&](std::vector<TagAdsFailure> result) { last_result = std::move(result); };
    auto pump = [&] {
        size_t count = 0;
        while (!jobs.empty() && count < 10) {
            auto job = std::move(jobs.front()); jobs.pop_front(); job(); ++count;
        }
        return count;
    };
    QueueTagAdsDrain(batch, enqueue, publish);
    Check(pump() == 2 && jobs.empty() && last_result.empty(), "65 items automatically drain across two serial jobs");
    for (int i = 65; i < 130; ++i) {
        wchar_t name[32]{}; swprintf_s(name, L"\\batch-%03d.txt", i);
        TagAdsUpdate update; update.path = fixture + name;
        created = pulse::WriteUtf8FileAtomic(update.path, L"private batch") && created;
        batch.push_back(std::move(update));
    }
    Check(created, "create additional exclusive batch files");
    QueueTagAdsDrain(batch, enqueue, publish);
    Check(pump() == 3 && jobs.empty() && last_result.empty(), "130 items automatically drain across three jobs");
    Check(WriteTagAdsV2(batch.front().path, {{L"t", L"locked", 0}}), "prepare failed head ADS");
    held = CreateFileW((batch.front().path + L":Pulse.Tag").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "lock failed head ADS");
    QueueTagAdsDrain(batch, enqueue, publish);
    Check(pump() == 3 && jobs.empty() && last_result.size() == 1 &&
        last_result[0].path == batch.front().path && last_result[0].error == ERROR_SHARING_VIOLATION &&
        !HasPendingTagAds(batch.back().path), "failed first entry cannot starve later batches");
    Check(pump() == 0 && HasPendingTagAds(batch.front().path), "persistent failure stops without spinning and retains intent");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    QueueTagAdsDrain({}, enqueue, publish);
    Check(pump() == 1 && last_result.empty(), "next trigger retries retained failure after release");
    QueueTagAdsDrain(batch, enqueue, publish);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); }
    Check(!jobs.empty() && !last_result.empty(), "batch schedules continuation with durable pending work");
    jobs.clear(); // WorkerPool shutdown rejects/destroys queued serial continuations.
    Check(HasPendingTagAds(batch.back().path), "cancelled continuation preserves pending journal");
    QueueTagAdsDrain({}, enqueue, publish);
    Check(pump() == 2 && last_result.empty() && pulse::ReadUtf8File(journal, journal_text) &&
        journal_text == L"PULSE_ADS_PENDING_1\n", "startup trigger resumes cancelled drain to durable completion");
    fixture = (dir / L"bad-profile").wstring(); std::filesystem::create_directory(fixture);
    Check(pulse::WriteUtf8FileAtomic(fixture + L"\\tag_ads_pending.dat", L"invalid"), "create malformed private journal");
    Check(!SyncTagAdsUpdates({removal}, &diagnostics).empty() && diagnostics.size() == 1 &&
        diagnostics[0].stage == TagAdsFailureStage::JournalRead && !diagnostics[0].unsupported,
        "malformed journal reports read stage without unsupported claim");
    TagAdsNoticeState notice;
    std::wstring title, message;
    const TagAdsFailure first{file, TagAdsFailureStage::StreamWrite, ERROR_SHARING_VIOLATION};
    const TagAdsFailure second{file + L"-other", TagAdsFailureStage::StreamWrite, ERROR_ACCESS_DENIED};
    ApplyTagAdsNotice(notice, {first}, title, message, true);
    Check(!message.empty() && message == TagAdsFailureMessage(first), "failure installs owned sync banner");
    ApplyTagAdsNotice(notice, {second}, title, message, false);
    Check(message == TagAdsFailureMessage(second), "remaining pending failure survives resolution of first even on background page");
    ApplyTagAdsNotice(notice, {}, title, message, false);
    Check(title.empty() && message.empty(), "successful sync clears resolved owned banner on background page");
    ApplyTagAdsNotice(notice, {first}, title, message, true);
    title = L"Unrelated notification"; message = L"Keep this warning";
    ApplyTagAdsNotice(notice, {}, title, message, true);
    Check(title == L"Unrelated notification" && message == L"Keep this warning",
        "successful sync cannot clear unrelated replacement banner");
    ApplyTagAdsNotice(notice, {first}, title, message, true);
    Check(message == L"Keep this warning", "sync retry cannot overwrite unrelated banner");
    notice = {}; title.clear(); message.clear();
    const TagAdsFailure old_path{L"Z:\\old-folder\\moved.txt", TagAdsFailureStage::StreamWrite, ERROR_PATH_NOT_FOUND};
    ApplyTagAdsNotice(notice, {old_path}, title, message, true, L"D:\\Music");
    Check(title.empty() && message.empty(), "unrelated folder does not display old path retry");
    const TagAdsFailure local{L"D:\\Music\\track.mp3", TagAdsFailureStage::StreamWrite, ERROR_ACCESS_DENIED};
    ApplyTagAdsNotice(notice, {old_path, local}, title, message, true, L"d:\\music\\");
    Check(message == TagAdsFailureMessage(local), "current folder still displays genuine sync failure");
    ApplyTagAdsNotice(notice, {local}, title, message, true, L"D:\\Music2");
    Check(title.empty() && message.empty(), "changing folders clears the owned old banner");
    ApplyTagAdsNotice(notice, {{L"D:\\Music2\\track.mp3", TagAdsFailureStage::StreamWrite, ERROR_ACCESS_DENIED}},
        title, message, true, L"D:\\Music");
    Check(title.empty() && message.empty(), "folder boundary prevents sibling prefix match");
    ApplyTagAdsNotice(notice, {{fixture, TagAdsFailureStage::JournalRead, ERROR_INVALID_DATA}},
        title, message, true, L"D:\\Music");
    Check(!message.empty(), "global journal failure remains visible in current folder");
    uint64_t revision = 0;
    Check(AcceptTagAdsResult(revision, 2) && !AcceptTagAdsResult(revision, 1) &&
        !AcceptTagAdsResult(revision, 2) && AcceptTagAdsResult(revision, 3),
        "late and duplicate snapshots cannot override newer sync result");
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    Check(!error, "remove only exclusive private fixture");
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--folder-bottom") {
        pulse::app::AppPrefs prefs, reloaded;
        Check(prefs.FromJson(LR"({"folder_sort_mode":1,"sort_folders_follow":["C:\\Legacy"],"sort_folders_top":["C:\\Top"],"sort_folders_mixed":["C:\\Mixed"]})") &&
            prefs.folder_sort_mode == 1 && prefs.folder_sorts.FindFolderMode(L"C:\\Legacy") == 1 &&
            prefs.folder_sorts.FindFolderMode(L"C:\\Top") == 0 && prefs.folder_sorts.FindFolderMode(L"C:\\Mixed") == 2,
            "legacy follow mode migrates to bottom without changing other modes");
        const auto json = prefs.ToJson();
        Check(json.find(L"sort_folders_bottom") != std::wstring::npos && json.find(L"sort_folders_follow") == std::wstring::npos &&
            reloaded.FromJson(json) && reloaded.folder_sorts.FindFolderMode(L"C:\\Legacy") == 1 && reloaded.folder_sort_mode == 1,
            "bottom persists with explicit key and round trips");
        Check(reloaded.FromJson(LR"({"sort_folders_follow":["C:\\Both"],"sort_folders_top":["C:\\Both"]})") &&
            reloaded.folder_sorts.FindFolderMode(L"C:\\Both") == 0, "explicit current mode overrides legacy mode");
        return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--native-command-pins") {
        using namespace pulse::app;
        const std::wstring native = L"native:sample.archive:extract";
        AppPrefs prefs, reloaded;
        prefs.persist = reloaded.persist = false;
        prefs.toolbar_tools = {L"cut", L"|", native, L"::right", L"copy"};
        Check(reloaded.FromJson(prefs.ToJson()) && reloaded.toolbar_tools == prefs.toolbar_tools,
            "native command pin identity and toolbar position survive preference reload");
        Check(ToolbarToolsOnly(reloaded.toolbar_tools) == std::vector<std::wstring>{L"cut", native, L"copy"} &&
            ToolbarRightFrom(reloaded.toolbar_tools) == 2 && ToolbarSeparatorMask(reloaded.toolbar_tools) == 2,
            "native command pins count in layout and separator slots");
        Check(ToolbarToolsOnly(MoveToolbarTool(reloaded.toolbar_tools, 1, 0)) ==
            std::vector<std::wstring>{native, L"cut", L"copy"}, "native command pin reorders with builtin tools");
        Check(ToolbarToolsOnly(RemoveToolbarTool(reloaded.toolbar_tools, 1)) ==
            std::vector<std::wstring>{L"cut", L"copy"}, "native command pin removal preserves builtin tools");
        Check(NormalizeToolbarTools({native, native, L"native::x", L"native:x:", L"native:x:y:z", L"native:x:/path"}) ==
            std::vector<std::wstring>{native}, "native pin IDs reject malformed identities and duplicates");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--toolbar-view-order") {
        using pulse::app::AppPrefs;
        const std::vector<std::wstring> expected{L"group", L"sort", L"filter"};
        AppPrefs prefs, reloaded;
        prefs.toolbar_view_order = expected;
        prefs.toolbar_show_sort = false;
        Check(reloaded.FromJson(prefs.ToJson()) && reloaded.toolbar_view_order == expected && !reloaded.toolbar_show_sort,
            "toolbar view order and visibility round trip independently");
        Check(reloaded.FromJson(L"{}") && reloaded.toolbar_view_order == std::vector<std::wstring>{L"sort", L"filter", L"group"},
            "older preferences retain original toolbar order");
        Check(reloaded.FromJson(LR"({"toolbar_view_order":["group","unknown","group","sort"]})") &&
            reloaded.toolbar_view_order == expected, "invalid view order is normalized without losing valid choices");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--tag-ads") return TagAdsOnly();
    using namespace pulse; using namespace pulse::app;
    auto dir = std::filesystem::absolute(L"bench_data/persistence-regression-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir); fixture = dir.wstring();
    const auto config = fixture + L"\\app.json";
    AppPrefs original; original.background_image = L"original.png";
    Check(original.Save(), "fixture saved");
    std::wstring before; ReadUtf8File(config, before);
    HANDLE locked = CreateFileW(config.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    AppPrefs prefs; prefs.persist = false;
    Check(!prefs.Load() && prefs.load_failed, "PERSIST-01 existing locked configuration records failure");
    CloseHandle(locked); prefs.persist = true;
    Check(!prefs.Save(), "PERSIST-01 unlock alone cannot overwrite with defaults");
    std::wstring after; ReadUtf8File(config, after);
    Check(before==after, "PERSIST-01 original bytes retained");
    prefs.persist=false;
    Check(prefs.Load() && !prefs.load_failed && prefs.background_image==L"original.png", "PERSIST-01 successful reread restores settings");
    WriteUtf8FileAtomic(config, L"{\"background_image\":");
    Check(!prefs.Load() && prefs.load_failed, "PERSIST-01 malformed configuration rejected");
    prefs.persist=true; Check(!prefs.Save(), "PERSIST-01 malformed config protected");
    WriteUtf8FileAtomic(config,before); prefs.persist=false; prefs.Load(); prefs.persist=true;
    LayoutTabSnapshot saved; saved.layout=1; saved.focused=1; saved.target=0;
    PaneFolderSnapshot pc; PaneFolderSnapshot disk; disk.path=L"C:\\";
    saved.panes={pc,disk}; LayoutTab tab;
    RestoreLayoutTab(tab,saved,[](Tab& view,const std::wstring& path){view.current_path=path;});
    Check(tab.panes.size()==2 && tab.panes[0]->ActiveTab()->current_path.empty() && tab.panes[1]->ActiveTab()->current_path==L"C:\\" && tab.focused_index==1 && tab.target_index==0, "PERSIST-02 This PC preserves pane order focus and target");
    const std::wstring places_file=fixture+L"\\places.json";
    const std::wstring json=LR"({"workspaces":[{"name":"a}b\"c","root":"C:\\project}2026","layout":0,"panes":["C:\\project}2026"]},{"name":"next","root":"C:\\next"}],"tags":[{"id":"t","name":"tag}x","rgb":0xABCDEF,"paths":[]}],"networks":[{"name":"n}x","unc":"\\\\host\\share}"}]})";
    WriteUtf8FileAtomic(places_file,json);
    {
        PlacesCatalog places; places.persist=false;
        Check(places.Load() && places.workspaces.size()==2 && places.workspaces[0].name==L"a}b\"c" && places.tags[0].name==L"tag}x" && places.networks[0].name==L"n}x", "PERSIST-03 quoted braces escapes and following records load");
        places.persist=true; Check(places.Save(), "PERSIST-03 round trip saves"); places.persist=false;
        Check(places.Load() && places.workspaces.size()==2 && places.workspaces[0].name==L"a}b\"c", "PERSIST-03 round trip retains records");
        WriteUtf8FileAtomic(places_file,L"{broken");
        Check(!places.Load() && places.load_failed && places.workspaces.size()==2, "PERSIST-03 corrupt reload retains live catalog");
        places.persist=true; Check(!places.Save(), "PERSIST-03 corrupt file cannot be overwritten");
        places.persist=false;
    }
    IoTaskQueue queue; std::wstring order;
    queue.Push([&]{order+=L"add";},true);queue.Push([&]{order+=L"remove";},true);
    auto first=queue.Pop();Check(!queue.Ready(), "PERSIST-04 second ADS update cannot overtake active update");
    queue.Push([&]{order+=L"other";},false);auto parallel=queue.Pop();parallel.task();queue.Complete(parallel);
    first.task();queue.Complete(first);auto second=queue.Pop();second.task();queue.Complete(second);
    Check(order==L"otheraddremove", "PERSIST-04 serial FIFO keeps unrelated IO available");
    const auto file=fixture+L"\\tagged.txt";WriteUtf8FileAtomic(file,L"fixture");
    Check(WriteTagAdsV2(file,{{L"t",L"label",0xABCDEF}}), "PERSIST-04 create ADS");
    locked=CreateFileW((file+L":Pulse.Tag").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(!WriteTagAdsV2(file,{}), "PERSIST-04 deletion failure reported");
    TagAdsUpdate removal; removal.path=file;
    Check(!SyncTagAdsUpdates({removal}).empty() && HasPendingTagAds(file), "PERSIST-04 failed delete retains authoritative pending intent");
    const auto fixture_saved=fixture;fixture+=L"\\other";HasPendingTagAds(file);fixture=fixture_saved;
    Check(HasPendingTagAds(file), "PERSIST-04 pending intent survives journal reload");
    { PlacesCatalog catalog;catalog.persist=false;
      catalog.MergeAdsRecords(file,{{L"old",L"stale",0}},{});
      Check(catalog.tags.empty(), "PERSIST-04 pending path rejects stale ADS reimport"); }
    CloseHandle(locked);
    Check(SyncTagAdsUpdates({}).empty() && !HasPendingTagAds(file), "PERSIST-04 retry clears pending intent after success");
    Check(WriteTagAdsV2(file,{}) && ReadTagAdsV2(file).empty(), "PERSIST-04 delete retry removes ADS");
    const auto old=fixture+L"\\wallpaper.png"; WriteUtf8FileAtomic(old,L"old-image");prefs.background_image=old;prefs.Save();
    Check(!prefs.StoreBackgroundImage(fixture+L"\\missing.png") && prefs.background_image==old && std::filesystem::exists(old), "PERSIST-05 missing source preserves previous image");
    const auto source=fixture+L"\\new.png";WriteUtf8FileAtomic(source,L"new-image");
    locked=CreateFileW(config.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    Check(!prefs.StoreBackgroundImage(source) && prefs.background_image==old && std::filesystem::exists(old), "PERSIST-05 configuration commit failure preserves previous image");CloseHandle(locked);
    Check(prefs.StoreBackgroundImage(source) && std::filesystem::exists(prefs.background_image) && !std::filesystem::exists(old), "PERSIST-05 successful commit precedes old image removal");
    AppPrefs reloaded;reloaded.persist=false;Check(reloaded.Load() && reloaded.background_image==prefs.background_image, "PERSIST-05 new cache path survives reload");
    {
        // 快速预览 > 预览窗口 / 按住预览 (task 54): defaults, round trip, bad values.
        AppPrefs older; older.persist=false;
        Check(older.FromJson(LR"({"hover_card_size":2})") && older.preview_size==0 && older.preview_remember_size &&
              older.preview_rect_w==0 && older.preview_space_closes && older.preview_loop==0 &&
              older.hold_preview_ms==0 && older.preview_key_hint_left==3 && older.hover_card_size==2,
              "PREVIEW-01 prefs from before keep the defaults (fit, remember, Space closes, hold off)");
        AppPrefs set; set.persist=false;
        set.preview_size=4; set.preview_remember_size=false; set.preview_rect_x=-1500; set.preview_rect_y=40;
        set.preview_rect_w=1200; set.preview_rect_h=800; set.preview_space_closes=false; set.preview_loop=1;
        set.hold_preview_ms=500; set.preview_key_hint_left=1; set.hover_card_size=3;
        AppPrefs set_again; set_again.persist=false;
        Check(set_again.FromJson(set.ToJson()) && set_again.preview_size==4 && !set_again.preview_remember_size &&
              set_again.preview_rect_x==-1500 && set_again.preview_rect_y==40 && set_again.preview_rect_w==1200 &&
              set_again.preview_rect_h==800 && !set_again.preview_space_closes && set_again.preview_loop==1 &&
              set_again.hold_preview_ms==500 && set_again.preview_key_hint_left==1 && set_again.hover_card_size==3,
              "PREVIEW-02 preview window prefs and the extra large card round trip");
        AppPrefs bad; bad.persist=false;
        Check(bad.FromJson(LR"({"preview_size":9,"preview_loop":-2,"hold_preview_ms":450,"preview_key_hint_left":40,"preview_rect_w":-5,"hover_card_size":7})") &&
              bad.preview_size==0 && bad.preview_loop==0 && bad.hold_preview_ms==0 && bad.preview_key_hint_left==3 &&
              bad.preview_rect_w==0 && bad.hover_card_size==1,
              "PREVIEW-03 out-of-range preview values fall back to the defaults");
    }
    {
        using Tools = std::vector<std::wstring>;
        AppPrefs legacy; legacy.persist=false;
        Check(legacy.FromJson(LR"({"toolbar_pins":5})") && legacy.toolbar_tools==Tools{L"cut",L"copy",L"paste",L"rename",L"delete",L"new_text",L"terminal",L"split",L"details",L"columns"}, "TOOLBAR-01 legacy pin mask becomes the tool list, with the former fixed buttons");
        legacy.toolbar_tools={L"hidden",L"copy_path",L"bogus",L"hidden",L"theme"};
        const std::wstring saved_json=legacy.ToJson();
        AppPrefs again; again.persist=false;
        Check(again.FromJson(saved_json) && again.toolbar_tools==Tools{L"hidden",L"copy_path",L"theme"}, "TOOLBAR-02 tool list round trips; unknown and repeated ids dropped");
        Check(saved_json.find(L"\"toolbar_pins\":2")!=std::wstring::npos, "TOOLBAR-03 an older Pulse still reads its pins");
        AppPrefs cleared; cleared.persist=false;
        Check(cleared.FromJson(LR"({"toolbar_tools":[],"toolbar_tools_v":2,"toolbar_pins":31})") && cleared.toolbar_tools.empty(), "TOOLBAR-04 an emptied list is not refilled from the old mask");
        Check(cleared.toolbar_selection==pulse::app::DefaultToolbarSelection(), "TOOLBAR-05 prefs without toolbar_selection show every chip action");
        AppPrefs chip; chip.persist=false;
        chip.toolbar_selection={L"tags",L"bogus",L"compare",L"tags"};
        AppPrefs chip_again; chip_again.persist=false;
        Check(chip_again.FromJson(chip.ToJson()) && chip_again.toolbar_selection==Tools{L"tags",L"compare"}, "TOOLBAR-06 chip action list round trips; unknown and repeated ids dropped");
        AppPrefs chip_none; chip_none.persist=false;
        Check(chip_none.FromJson(LR"({"toolbar_selection":[]})") && chip_none.toolbar_selection.empty(), "TOOLBAR-07 an emptied chip list stays empty");
        AppPrefs v1; v1.persist=false;
        Check(v1.FromJson(LR"({"toolbar_tools":["zebra","details"]})") &&
              v1.toolbar_tools==Tools{L"cut",L"copy",L"paste",L"rename",L"delete",L"zebra",L"details",L"split",L"columns"},
              "TOOLBAR-08 a list from before the full catalog gets the former fixed buttons back, once");
        AppPrefs v1_again; v1_again.persist=false;
        Check(v1_again.FromJson(v1.ToJson()) && v1_again.toolbar_tools==v1.toolbar_tools, "TOOLBAR-09 the upgraded list round trips unchanged");
        AppPrefs fresh; fresh.persist=false;
        Check(fresh.toolbar_tools==pulse::app::DefaultToolbarTools() && fresh.toolbar_tools.size()==10 &&
              pulse::app::ToolbarToolsOnly(fresh.toolbar_tools).size()==9 && fresh.toolbar_tools[5]==L"|",
              "TOOLBAR-10 a fresh install gets the prototype's default tools, in two groups");
        AppPrefs sep; sep.persist=false;
        sep.toolbar_tools={L"|",L"cut",L"|",L"|",L"copy",L"bogus",L"|",L"|"};
        AppPrefs sep_again; sep_again.persist=false;
        Check(sep_again.FromJson(sep.ToJson()) && sep_again.toolbar_tools==Tools{L"cut",L"|",L"copy"},
              "TOOLBAR-11 separators round trip; none first, last or twice in a row");
        using namespace pulse::app;
        const Tools grouped{L"cut",L"copy",L"|",L"paste",L"rename"};
        Check(ToolbarSeparatorMask(grouped)==4u && MoveToolbarTool(grouped,0,3)==Tools{L"copy",L"paste",L"|",L"rename",L"cut"},
              "TOOLBAR-12 dragging a tool keeps the separator between the same slots");
        Check(RemoveToolbarTool(grouped,2)==Tools{L"cut",L"copy",L"|",L"rename"} &&
              RemoveToolbarTool(grouped,1)==Tools{L"cut",L"|",L"paste",L"rename"},
              "TOOLBAR-13 removing a tool next to a separator keeps the separator");
        Check(SetToolbarSeparatorAfter(grouped,2,true)==Tools{L"cut",L"copy",L"|",L"paste",L"|",L"rename"} &&
              SetToolbarSeparatorAfter(grouped,1,false)==Tools{L"cut",L"copy",L"paste",L"rename"} &&
              SetToolbarSeparatorAfter(grouped,3,true)==grouped,
              "TOOLBAR-14 a separator is added or removed after a tool, never after the last");
        Check(KeepToolbarSeparators(grouped,Tools{L"cut",L"copy",L"rename",L"zip"})==Tools{L"cut",L"copy",L"|",L"rename",L"zip"},
              "TOOLBAR-15 a picker change keeps a separator before the next remaining tool");
    }
    {
        // #58-9: each folder-picker entry point remembers its own folder.
        using pulse::app::AppPrefs;
        AppPrefs picker; picker.persist = false;
        Check(picker.PickerFolder(L"tray_send").empty(), "PICKER-01 no history: empty, so the picker uses its own last folder");
        const bool stored = picker.RememberPickerFolder(L"tray_send", L"D:\\Out") &&
            picker.RememberPickerFolder(L"duplicates", L"E:\\Photos");
        Check(stored && picker.PickerFolder(L"tray_send") == L"D:\\Out" && picker.PickerFolder(L"duplicates") == L"E:\\Photos",
              "PICKER-02 entry points keep separate folders");
        Check(!picker.RememberPickerFolder(L"duplicates", L"E:\\Photos") && picker.picker_folders.size() == 2,
              "PICKER-03 the same pick again adds nothing");
        picker.RememberPickerFolder(L"tray_send", L"D:\\Later");
        Check(picker.PickerFolder(L"tray_send") == L"D:\\Later" && picker.picker_folders.size() == 2 &&
              picker.picker_folders.front() == L"tray_send|D:\\Later", "PICKER-04 a newer pick replaces the old one, newest first");
        Check(picker.PickerFolder(L"tray").empty() && picker.PickerFolder(L"tray_send|").empty(),
              "PICKER-05 keys match whole");
        Check(!picker.RememberPickerFolder(L"", L"C:\\x") && !picker.RememberPickerFolder(L"a|b", L"C:\\x") &&
              !picker.RememberPickerFolder(L"k", L"") && picker.picker_folders.size() == 2,
              "PICKER-06 empty or malformed entries are ignored");
        AppPrefs again; again.persist = false;
        Check(again.FromJson(picker.ToJson()) && again.picker_folders == picker.picker_folders &&
              again.PickerFolder(L"duplicates") == L"E:\\Photos", "PICKER-07 the history round trips");
        AppPrefs bad; bad.persist = false;
        Check(bad.FromJson(LR"({"picker_folders":["nobar","|C:\\x","k|","ok|C:\\y"]})") &&
              bad.picker_folders == std::vector<std::wstring>{L"ok|C:\\y"}, "PICKER-08 malformed lines are dropped on load");
        AppPrefs many; many.persist = false;
        for (int i = 0; i < 40; ++i) many.RememberPickerFolder(L"k" + std::to_wstring(i), L"C:\\f");
        Check(many.picker_folders.size() == pulse::app::kPickerFolderLimit && many.PickerFolder(L"k39") == L"C:\\f" &&
              many.PickerFolder(L"k0").empty(), "PICKER-09 capped; the oldest entry point is dropped");
    }
    {
        // Folders-first is remembered per folder, like the sort column; folders
        // without a choice follow Settings > Folders first (folder_sort_mode).
        using pulse::app::AppPrefs;
        AppPrefs modes; modes.persist = false;
        modes.folder_sort_mode = 0;
        auto& sorts = modes.folder_sorts;
        Check(sorts.SetFolderMode(L"C:\\Work", 2) && !sorts.SetFolderMode(L"C:\\Work", 2) &&
              sorts.SetFolderMode(L"D:\\Media\\", 1), "FOLDERS-01 a folder keeps its own folders-first mode");
        Check(sorts.FindFolderMode(L"c:\\work") == 2 && sorts.FindFolderMode(L"D:\\Media") == 1 &&
              !sorts.FindFolderMode(L"C:\\Other") && !sorts.FindFolderMode(L"C:\\Work\\Sub"),
              "FOLDERS-02 only that folder (any case, trailing slash); others use the settings default");
        Check(!sorts.SetFolderMode(L"pulse:recent", 2) && !sorts.SetFolderMode(L"", 1) &&
              !sorts.SetFolderMode(L"C:\\Work", 3) && !sorts.SetFolderMode(L"C:\\Work", -1) &&
              sorts.FindFolderMode(L"C:\\Work") == 2, "FOLDERS-03 virtual views and bad modes are not stored");
        sorts.Set(L"C:\\Work", { pulse::ui::SortColumn::Mtime, pulse::ui::SortDirection::Desc });
        AppPrefs reread; reread.persist = false;
        Check(reread.FromJson(modes.ToJson()) && reread.folder_sorts.FindFolderMode(L"C:\\Work") == 2 &&
              reread.folder_sorts.FindFolderMode(L"D:\\Media") == 1 && !reread.folder_sorts.FindFolderMode(L"C:\\Other") &&
              reread.folder_sort_mode == 0 && reread.folder_sorts.Find(L"C:\\Work") ==
                  pulse::app::FolderSort{ pulse::ui::SortColumn::Mtime, pulse::ui::SortDirection::Desc },
              "FOLDERS-04 per-folder modes round trip with the sort column and the settings default");
        reread.folder_sorts.ApplyToAll({});
        Check(!reread.folder_sorts.FindFolderMode(L"C:\\Work") && !reread.folder_sorts.FindFolderMode(L"D:\\Media") &&
              !reread.folder_sorts.Find(L"C:\\Work"), "FOLDERS-05 apply to all folders forgets per-folder modes too");
        AppPrefs older; older.persist = false;
        Check(older.FromJson(LR"({"folder_sort_mode":2,"sort_by_folder_name_desc":["C:\\A"]})") &&
              older.folder_sort_mode == 2 && !older.folder_sorts.FindFolderMode(L"C:\\A"),
              "FOLDERS-06 an older settings file: every folder follows the settings default");
    }
    {
        // #58-7b: picture folders without a saved view open in Large icons.
        using pulse::app::IsPictureName;
        using pulse::app::MostlyPictures;
        Check(IsPictureName(L"IMG_0001.JPG") && IsPictureName(L"a.webp") && IsPictureName(L"shot.HEIC") &&
              IsPictureName(L"raw.CR3") && IsPictureName(L"x.dng"), "PICTURE-01 photo and camera raw extensions, any case");
        Check(!IsPictureName(L"notes.txt") && !IsPictureName(L"jpg") && !IsPictureName(L".png") &&
              !IsPictureName(L"movie.mp4") && !IsPictureName(L"a.jpgxyzw"), "PICTURE-02 other files and bare names are not pictures");
        Check(MostlyPictures(10, 6) && MostlyPictures(6, 6) && !MostlyPictures(10, 5),
              "PICTURE-03 60% of the files or more");
        Check(!MostlyPictures(5, 5) && !MostlyPictures(0, 0) && !MostlyPictures(3, 7),
              "PICTURE-04 fewer than six pictures (or bad counts) never switch");
    }
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
