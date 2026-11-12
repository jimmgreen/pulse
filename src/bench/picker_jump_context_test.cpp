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
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File; Check(Ready(context, spec) && spec.recent_files.size() == 1 && spec.recent_files.front().path == accepted_file.wstring(), "recent files contain actual accepted same-app file");
        Check(!spec.recent_files.empty() && spec.recent_files.front().detail.starts_with(root.wstring()) &&
            spec.recent_files.front().detail.size() > root.wstring().size(), "recent card contains actual parent directory and visit date");
        Check(!spec.app_recent_places.empty() && !spec.app_recent_places.front().detail.empty(), "recent directory shows actual visit date");
        Check(!spec.last_location_source.empty() && !spec.recent_files_caption.empty(), "history chips and cards identify their actual app source");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File; spec.filters = {{L"Images", L"*.png"}, {L"Text", L"*.txt"}};
        Check(Ready(context, spec) && spec.recent_files.empty(), "recent files honor active file filter");
        spec.filter_index = 1;
        Check(context.Poll(spec) && spec.recent_files.size() == 1, "filter switching updates recent cards from memory");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::filesystem::remove(accepted_file);
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File; Check(Ready(context, spec) && spec.recent_files.empty(), "deleted recent file is omitted");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto pulse_file = root / L"pulse-recent.txt";
    { std::ofstream stream(pulse_file); stream << "pulse recent fixture"; }
    auto merged_options = options;
    merged_options.pulse_places_file = (root / L"places.json").wstring();
    std::wstring escaped; json::Escape(pulse_file.wstring(), escaped);
    const auto ticks = (static_cast<uint64_t>(app::JumpHistory::Now() - 600) + 11644473600ULL) * 10000000ULL;
    WriteUtf8FileAtomic(merged_options.pulse_places_file, L"{\"recent_items\":[{\"path\":\"" + escaped + L"\",\"kind\":\"file\",\"opened_at\":\"" + std::to_wstring(ticks) + L"\"}]}");
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), merged_options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File;
        Check(Ready(context, spec) && spec.recent_files.size() == 1 && spec.recent_files[0].path == pulse_file.wstring(),
            "recent cards merge real Pulse places JSON file records");
        Check(!spec.recent_files.empty() && spec.recent_files[0].detail.find(L"10 minutes ago") != std::wstring::npos,
            "recent card uses actual relative minutes");
        context.Accepted(pulse_file.wstring());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), merged_options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File;
        Check(Ready(context, spec) && spec.recent_files.size() == 1, "Pulse and same-app duplicate paths merge once");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    merged_options.read_places = [shared] { auto disabled = shared; disabled.remember = false; return disabled; };
    {
        picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), merged_options);
        ui::FolderPickerSpec spec; spec.mode = ui::PickerMode::File;
        Check(Ready(context, spec) && spec.recent_files.empty(), "privacy off suppresses Pulse and app recent files together");
    }
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
