#include "../picker_inprocess/picker_jump_context.h"
#include "../ui/folder_picker_dialog.h"
#include "../app/jump_history.h"
#include "../common/dialog_bundle.h"
#include "../common/localization.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <thread>
#include <cstdio>
#include <fstream>
#include <atomic>
#include <mutex>
namespace pulse::picker_inprocess {
std::wstring FormatJumpVisitDate(int64_t seconds, int64_t now);
std::wstring ResolveJumpIndexHelper(const std::wstring&, dialog_bundle::BundleLease&);
}
using namespace pulse;
namespace {
int failures = 0;
void Check(bool okay, const char* message) { std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", message); if (!okay) ++failures; }
bool Ready(picker_inprocess::JumpContext& context, ui::FolderPickerSpec& spec) {
    for (int i = 0; i < 100; ++i) { if (context.Poll(spec)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return false;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--last-location") {
        const auto root = std::filesystem::absolute(argv[2]);
        if (std::filesystem::exists(root)) return 3;
        std::filesystem::create_directories(root);
        picker_inprocess::JumpContextOptions options;
        options.enumerate_explorer = false;
        options.history_root = root.wstring();
        options.read_places = [] { picker_places::Snapshot s; s.jump_policy_available = true; s.remember = true; return s; };
        for (const auto mode : {ui::PickerMode::File, ui::PickerMode::Save, ui::PickerMode::Folder}) {
            const auto directory = root / std::to_wstring(static_cast<int>(mode));
            std::filesystem::create_directory(directory);
            const auto accepted = mode == ui::PickerMode::Folder ? directory : directory / L"result.txt";
            if (mode != ui::PickerMode::Folder) std::ofstream(accepted) << "fixture";
            {
                picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(mode), options);
                ui::FolderPickerSpec spec;
                Check(Ready(context, spec), "history producer becomes ready without Explorer enumeration");
                context.Accepted(accepted.wstring());
            }
            bool committed = false;
            for (int i=0;i<150&&!committed;++i) {
                for (const auto& file : std::filesystem::directory_iterator(root)) {
                    if (file.path().extension() != L".tsv") continue;
                    app::JumpHistory history; history.Open(file.path().wstring());
                    committed |= history.LastUsed(directory.wstring()) > 0;
                }
                if (!committed) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Check(committed, "actual accepted location reaches isolated history file");
            for (const auto* language : {L"zh-CN", L"en-US", L"zh-TW"}) {
                l10n::Initialize(GetModuleHandleW(nullptr), language);
                picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(mode), options);
                ui::FolderPickerSpec spec;
                for(int i=0;i<150&&spec.last_location.path.empty();++i) {
                    context.Poll(spec);
                    if(spec.last_location.path.empty())std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                Check(spec.last_location.path == directory.wstring(), "history reload preserves each mode's exact directory");
                auto expected = std::wstring(l10n::Get(mode == ui::PickerMode::Save ?
                    l10n::StringId::PickerAppLastSaveLocation : l10n::StringId::PickerAppLastLocation));
                const auto name = expected.find(L"{name}");
                if(name != expected.npos)expected.replace(name,6,spec.host_app_name);
                Check(name != expected.npos && !spec.host_app_name.empty() && spec.last_location_source == expected,
                    "production history caption identifies program and open/save action in all languages");
            }
        }
        std::printf("FAILURES=%d\n",failures);
        return failures?1:0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--forget") {
        // 常用 × removes a folder from this program's history; one folder reached
        // through a junction is recorded once, at its real location.
        const auto root = std::filesystem::absolute(argv[2]);
        if (std::filesystem::exists(root)) return 3;
        std::filesystem::create_directories(root);
        {
            app::JumpHistory memory;
            memory.Record(L"D:\\Fixture\\Print", {});
            Check(memory.Remove(L"d:\\fixture\\print\\"), "remove matches however the path is typed");
            Check(!memory.Remove(L"D:\\Fixture\\Print") && memory.Size() == 0, "a second remove finds nothing");
        }
        picker_inprocess::JumpContextOptions options;
        options.enumerate_explorer = false;
        options.history_root = root.wstring();
        options.read_places = [] { picker_places::Snapshot s; s.jump_policy_available = true; s.remember = true; return s; };
        const auto mode = static_cast<int>(ui::PickerMode::Folder);
        const auto keep = root / L"keep", drop = root / L"drop", target = root / L"target";
        for (const auto& d : {keep, drop, target}) std::filesystem::create_directory(d);
        const auto link = root / L"link";
        const std::wstring mklink = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
        const bool junction = _wsystem(mklink.c_str()) == 0 && std::filesystem::exists(link);
        const auto history_file = [&] {
            for (const auto& file : std::filesystem::directory_iterator(root))
                if (file.path().extension() == L".tsv") return file.path().wstring();
            return std::wstring();
        };
        const auto used = [&](const std::filesystem::path& folder) {
            const auto file = history_file();
            if (file.empty()) return int64_t{0};
            app::JumpHistory history; history.Open(file);
            return history.LastUsed(folder.wstring());
        };
        std::vector<std::filesystem::path> accepted = {keep, drop};
        if (junction) accepted.push_back(link);
        for (const auto& folder : accepted) {
            {
                picker_inprocess::JumpContext context(GetCurrentProcessId(), mode, options);
                ui::FolderPickerSpec spec;
                Check(Ready(context, spec), "history producer ready");
                context.Accepted(folder.wstring());
            }
            const auto expect = folder == link ? target : folder;
            for (int i = 0; i < 150 && !used(expect); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Check(used(expect) > 0, "accepted folder reaches the history file");
        }
        if (junction) {
            Check(used(target) > 0 && !used(link), "a junction is recorded at its real location");
        } else {
            std::printf("[SKIP] junction canonicalization: mklink /J unavailable here\n");
        }
        {
            picker_inprocess::JumpContext context(GetCurrentProcessId(), mode, options);
            ui::FolderPickerSpec spec;
            const auto has = [&](const std::filesystem::path& folder) {
                return std::any_of(spec.app_recent_places.begin(), spec.app_recent_places.end(),
                    [&](const auto& place) { return ui::SamePickerPath(place.path, folder.wstring()); });
            };
            for (int i = 0; i < 150 && !(has(keep) && has(drop)); ++i) {
                context.Poll(spec);
                if (!(has(keep) && has(drop))) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Check(has(keep) && has(drop), "both folders listed before the removal");
            context.Forget(drop.wstring());
            for (int i = 0; i < 150 && has(drop); ++i) {
                context.Poll(spec);
                if (has(drop)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Check(!has(drop) && has(keep), "the removed folder leaves the live list, the other stays");
        }
        for (int i = 0; i < 150 && used(drop); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(!used(drop) && used(keep) > 0, "the removal is written to the history file");
        {
            picker_inprocess::JumpContext context(GetCurrentProcessId(), mode, options);
            ui::FolderPickerSpec spec;
            Check(Ready(context, spec), "history reload after removal");
            for (int i = 0; i < 50; ++i) { context.Poll(spec); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            Check(std::none_of(spec.app_recent_places.begin(), spec.app_recent_places.end(),
                [&](const auto& place) { return ui::SamePickerPath(place.path, drop.wstring()); }), "a removed folder stays gone after reopening");
        }
        if (junction) RemoveDirectoryW(link.c_str());
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        Check(!ec, "isolated fixture removed");
        std::printf("FAILURES=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--selection") {
        l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
        struct Fixture { std::mutex mutex; picker_places::Snapshot snapshot; };
        auto fixture = std::make_shared<Fixture>();
        fixture->snapshot.selection_available = true;
        fixture->snapshot.selected_items = {{L"C:\\file.png", false, 32}, {L"C:\\folder", true, 0}};
        picker_inprocess::JumpContextOptions options;
        options.enumerate_explorer = false;
        options.read_places = [fixture] { std::lock_guard lock(fixture->mutex); return fixture->snapshot; };
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        ui::FolderPickerSpec spec;
        auto wait_for = [&](auto expected) {
            for (int i = 0; i < 150; ++i) {
                context.Poll(spec);
                if (expected()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        Check(wait_for([&] { return spec.pulse_selection_available && spec.pulse_selected_items.size() == 2; }),
            "selection worker delivers file and directory snapshot to FolderPickerSpec");
        Check(spec.pulse_selected_items.size() == 2 && !spec.pulse_selected_items[0].is_directory &&
            spec.pulse_selected_items[0].size == 32 && spec.pulse_selected_items[1].is_directory,
            "selection transport retains type and byte size without filesystem validation claims");
        { std::lock_guard lock(fixture->mutex); fixture->snapshot.selected_items.clear(); }
        Check(wait_for([&] { return spec.pulse_selection_available && spec.pulse_selected_items.empty(); }),
            "cleared source selection advances revision and clears picker data");
        { std::lock_guard lock(fixture->mutex); fixture->snapshot.selected_items = {{L"\\\\server\\share\\new.txt", false, 99}}; }
        Check(wait_for([&] { return spec.pulse_selected_items.size() == 1 && spec.pulse_selected_items[0].size == 99; }),
            "source change refreshes picker without requiring filter or search changes");
        { std::lock_guard lock(fixture->mutex); fixture->snapshot.selection_available = false; }
        Check(wait_for([&] { return !spec.pulse_selection_available && spec.pulse_selected_items.empty(); }),
            "unavailable source clears picker even if callback retains its old vector");
        return failures ? 1 : 0;
    }

    if (argc == 2 && std::wstring_view(argv[1]) == L"--slow-history") {
        l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto release = std::make_shared<std::atomic<bool>>(false);
        picker_inprocess::JumpContextOptions options;
        options.enumerate_explorer = false;
        options.read_places = [entered, release] {
            entered->store(true);
            while (!release->load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            picker_places::Snapshot snapshot;
            snapshot.jump_policy_available = true; snapshot.auto_jump = true;
            snapshot.current = L"C:\\fixture";
            return snapshot;
        };
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        for (int i = 0; i < 400 && !entered->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(entered->load(), "history worker reached blocked fixture");
        ui::FolderPickerSpec spec; spec.allow_auto_jump = true; spec.initial_path = L"C:\\original";
        Check(context.Poll(spec) && !spec.host_app_name.empty(), "branding publishes while history is blocked");
        Check(!context.Poll(spec), "unchanged branding does not repeatedly relayout");
        Check(spec.initial_path == L"C:\\original", "branding does not navigate");
        release->store(true);
        for (int i = 0; i < 400 && spec.initial_path != L"C:\\fixture"; ++i) {
            context.Poll(spec); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(spec.initial_path == L"C:\\fixture", "first history arrival retains auto jump");
        return failures ? 1 : 0;
    }
    if (argc != 2) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto local_seconds = [](WORD day) {
        SYSTEMTIME date{}; date.wYear = 2026; date.wMonth = 11; date.wDay = day; date.wHour = 12;
        FILETIME local{}, utc{}; SystemTimeToFileTime(&date, &local); LocalFileTimeToFileTime(&local, &utc);
        return static_cast<int64_t>(((static_cast<uint64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime) / 10000000ULL) - 11644473600LL;
    };
    const auto fixed_now = local_seconds(16);
    Check(picker_inprocess::FormatJumpVisitDate(fixed_now - 600, fixed_now) == L"10 minutes ago", "fixed clock relative minutes");
    Check(picker_inprocess::FormatJumpVisitDate(local_seconds(15), fixed_now) == l10n::Get(l10n::StringId::PickerYesterday),
        "fixed clock local yesterday");
    Check(picker_inprocess::FormatJumpVisitDate(local_seconds(13), fixed_now) == L"3 days ago", "fixed clock three local calendar days ago");
    Check(picker_inprocess::FormatJumpVisitDate(local_seconds(10), fixed_now) == L"6 days ago", "fixed clock six-day relative boundary");
    Check(picker_inprocess::FormatJumpVisitDate(local_seconds(9), fixed_now) == l10n::Get(l10n::StringId::GroupDateLastWeek),
        "fixed clock previous natural week");
    Check(picker_inprocess::FormatJumpVisitDate(local_seconds(2), fixed_now) != l10n::Get(l10n::StringId::GroupDateLastWeek),
        "fourteen days ago outside previous natural week stays an absolute date");
    app::JumpHistory metadata;
    metadata.Record(L"C:\\fixture", {}, 1700000000);
    Check(metadata.LastUsed(L"c:\\fixture") == 1700000000, "history metadata uses actual visit timestamp not decayed score");
    app::JumpHistory restored_metadata; restored_metadata.Parse(metadata.Serialize());
    Check(restored_metadata.LastUsed(L"C:\\fixture") == 1700000000, "visit timestamp survives existing history format");
    Check(restored_metadata.VisitCount(L"C:\\fixture") == 1, "exact new visit count survives serialization");
    restored_metadata.Record(L"C:\\fixture", {}, 1700000000 + 30 * 86400);
    Check(restored_metadata.VisitCount(L"C:\\fixture") == 2, "visit count does not decay with frecency");
    app::JumpHistory legacy;
    legacy.Parse(L"C:\\legacy\t12.5000\t1700000000\n");
    legacy.Record(L"C:\\legacy", {}, 1700000001);
    Check(legacy.VisitCount(L"C:\\legacy") == 0 && legacy.Score(L"C:\\legacy", {}, 1700000001) > 12,
        "legacy unknown cumulative count is never fabricated from score");
    restored_metadata.SetEnabled(false);
    Check(restored_metadata.VisitCount(L"C:\\fixture") == 0, "disabled history hides cumulative count");
    Check(restored_metadata.LastUsed(L"C:\\fixture") == 0, "disabled history does not expose metadata");
    const std::vector<ui::PickerJumpResult> mixed_results = {{L"C:\\folder", {}, {}, 3, false},
        {L"C:\\file.png", {}, {}, 3, true}, {L"C:\\file.txt", {}, {}, 3, true}};
    ui::PickerOptions result_filter; result_filter.filters = {{L"PNG", L"*.png"}, {L"Text", L"*.txt"}};
    auto filtered = picker_inprocess::FilterJumpResults(mixed_results, ui::PickerMode::Save, result_filter);
    Check(filtered.size() == 2 && filtered.back().path == L"C:\\file.png", "save index keeps folders and matching file type");
    result_filter.filter_index = 1;
    filtered = picker_inprocess::FilterJumpResults(mixed_results, ui::PickerMode::Save, result_filter);
    Check(filtered.size() == 2 && filtered.back().path == L"C:\\file.txt", "type switch filters old indexed files immediately");
    Check(picker_inprocess::FilterJumpResults(mixed_results, ui::PickerMode::Folder, result_filter).size() == 1,
        "folder picker never includes indexed files");
    picker_places::jump_policy_detail::Payload policy{0x314a5050, 1, 1, 15, 10000};
    Check(picker_places::jump_policy_detail::Valid(policy, 20000), "policy accepts exact freshness boundary");
    Check(!picker_places::jump_policy_detail::Valid(policy, 20001), "expired policy is rejected");
    Check(!picker_places::jump_policy_detail::Valid(policy, 9999), "future policy is rejected");
    policy.flags = 16; Check(!picker_places::jump_policy_detail::Valid(policy, 10000), "unknown policy flags are rejected");
    policy.flags = 15; policy.version = 2; Check(!picker_places::jump_policy_detail::Valid(policy, 10000), "unknown policy version is rejected");
    policy.version = 1; policy.pid = 0; Check(!picker_places::jump_policy_detail::Valid(policy, 10000), "missing policy publisher is rejected");
    const auto root = std::filesystem::absolute(argv[1]);
    if (std::filesystem::exists(root)) return 3;
    std::filesystem::create_directories(root);
    picker_places::Snapshot shared;
    shared.jump_policy_available = true; shared.auto_jump = true; shared.remember = true; shared.pinyin = true;
    shared.current = L"C:\\active";
    shared.folders = {{L"C:\\active", L"Active", true, false}, {L"C:\\季度报告", L"季度报告", false, true}};
    picker_inprocess::JumpContextOptions options;
    options.read_places = [shared] { return shared; }; options.history_root = root.wstring(); options.enumerate_explorer = false;
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Folder), options);
        ui::FolderPickerSpec spec; spec.initial_path = L"C:\\explicit";
        Check(Ready(context, spec), "background context arrives");
        Check(!spec.host_app_name.empty(), "real executable supplies name");
        Check(spec.initial_path == L"C:\\explicit" && spec.auto_jump_from.empty(), "explicit and legacy folder remains unchanged");
        Check(spec.open_folders.size() == 1 && spec.open_folders.front().path == shared.current, "only open Pulse rows become chips");
        context.Search(L"jd");
        std::vector<ui::PickerJumpResult> results;
        for (int i = 0; i < 100 && results.empty(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); results = context.Search(L"jd"); }
        Check(results.size() == 1 && results.front().group == 2, "pinyin favorites search resolves on worker");
        Check(context.Search(L"no-match").empty(), "new query never returns previous query results");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    {
        auto gate = std::make_shared<std::atomic<bool>>(false);
        auto delayed_options = options;
        delayed_options.read_places = [gate, shared] {
            while (!gate->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return shared;
        };
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Folder), delayed_options);
        context.Search(L"early-query");
        ui::FolderPickerSpec spec;
        Check(!context.Poll(spec), "early search revision cannot publish an unloaded history snapshot");
        gate->store(true);
        Check(Ready(context, spec) && spec.open_folders.size() == 1, "readiness waits for actual initial history and places loading");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(std::filesystem::is_empty(root), "cancel does not write history");
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Folder), options);
        ui::FolderPickerSpec spec; spec.initial_path = L"C:\\default"; spec.allow_auto_jump = true;
        Check(Ready(context, spec) && spec.initial_path == shared.current && spec.auto_jump_from == L"C:\\default", "default folder permits policy auto jump with undo origin");
        context.Accepted(L"C:\\accepted");
    }
    bool committed = false;
    for (int i = 0; i < 100 && !committed; ++i) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.path().extension() != L".tsv") continue;
            app::JumpHistory saved; saved.Open(entry.path().wstring());
            committed |= saved.LastUsed(L"C:\\accepted") > 0;
        }
        if (!committed) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Check(committed, "successful acceptance atomically commits actual history before reload");
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Folder), options);
        ui::FolderPickerSpec spec; Check(Ready(context, spec) && spec.last_location.path == L"C:\\accepted", "same program and mode restores accepted history");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    bool chronology_fixture_written = false, chronology_fixture_verified = false;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.path().filename().wstring().ends_with(L"-0.tsv")) continue;
        // Build in memory, commit once, then verify the exact disk snapshot used by the worker.
        app::JumpHistory fixture;
        const auto now = app::JumpHistory::Now();
        for (int i = 0; i < 10; ++i) fixture.Record(L"C:\\frequent-older", {}, now - 100);
        fixture.Record(L"C:\\last-visited", {}, now);
        chronology_fixture_written = WriteUtf8FileAtomic(entry.path().wstring(), fixture.Serialize());
        if (!chronology_fixture_written) std::printf("chronology fixture write error=%lu\n", GetLastError());
        app::JumpHistory saved; saved.Open(entry.path().wstring());
        chronology_fixture_verified = saved.LastUsed(L"C:\\last-visited") == now &&
            saved.LastUsed(L"C:\\frequent-older") == now - 100 && saved.VisitCount(L"C:\\frequent-older") == 10;
    }
    Check(chronology_fixture_written && chronology_fixture_verified, "chronology fixture fully committed and reopened before worker starts");
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Folder), options);
        ui::FolderPickerSpec spec;
        Check(Ready(context, spec) && spec.last_location.path == L"C:\\last-visited" &&
            !spec.app_recent_places.empty() && spec.app_recent_places.front().path == L"C:\\frequent-older",
            "last-location chip uses chronology while sidebar retains frequency ranking");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Save), options);
        ui::FolderPickerSpec spec; Check(Ready(context, spec) && spec.last_location.path.empty(), "different mode has independent history");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    shared.jump_policy_available = false; options.read_places = [shared] { return shared; };
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::Folder), options);
        ui::FolderPickerSpec spec; spec.initial_path = L"C:\\default"; spec.allow_auto_jump = true;
        Check(Ready(context, spec) && spec.initial_path == L"C:\\default" && spec.last_location.path.empty(), "missing old policy disables auto jump and history");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    shared.jump_policy_available = true; options.read_places = [shared] { return shared; };
    const auto accepted_file = root / L"accepted.txt";
    { std::ofstream stream(accepted_file); stream << "actual accepted file"; }
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        ui::FolderPickerSpec spec; Check(Ready(context, spec), "file-mode context arrives"); context.Accepted(accepted_file.wstring());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        // Accepted files remember their folder; the dialog no longer shows recent file cards.
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File; Check(Ready(context, spec), "file-mode history arrives");
        Check(!spec.app_recent_places.empty() && !spec.app_recent_places.front().detail.empty(), "recent directory shows actual visit date");
        Check(!spec.last_location_source.empty(), "history chips identify their actual app source");
    }
    std::filesystem::remove(accepted_file);
    const auto helper_root = root / L"resolver";
    std::filesystem::create_directories(helper_root);
    dialog_bundle::BundleLease lease;
    const auto module = helper_root / L"PulsePickerUiProof.dll";
    const auto helper = helper_root / L"Pulse.Index.exe";
    Check(picker_inprocess::ResolveJumpIndexHelper(module.wstring(), lease).empty(), "missing module sibling never becomes a bare helper name");
    { std::ofstream stream(helper); stream << "fixture only, never executed"; }
    Check(picker_inprocess::ResolveJumpIndexHelper(module.wstring(), lease) == helper.wstring(), "flat UI module resolves its own helper rather than caller exe directory");
    Check(picker_inprocess::ResolveJumpIndexHelper((helper_root / L"thirdparty.exe").wstring(), lease).empty(), "unrelated module does not supply a helper root");
    Check(picker_inprocess::ResolveJumpIndexHelper(L"PulsePickerUiProof.dll", lease).empty(), "relative module path is rejected");
    const auto malformed = helper_root / L"DialogBridge" / L"not-a-digest";
    std::filesystem::create_directories(malformed);
    Check(picker_inprocess::ResolveJumpIndexHelper((malformed / L"PulsePickerUiProof.dll").wstring(), lease).empty(), "malformed version directory never climbs to install root");
    const auto unverified = helper_root / L"DialogBridge" / std::wstring(64, L'a');
    std::filesystem::create_directories(unverified);
    Check(picker_inprocess::ResolveJumpIndexHelper((unverified / L"PulsePickerUiProof.dll").wstring(), lease).empty(), "unverified version bundle cannot use parent installation helper");
    return failures ? 1 : 0;
}
