#include "../index/change_tracking.h"
#include "../ipc/protocol.h"
#include <fstream>
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include <chrono>
using namespace pulse::index;
namespace pulse::index {
struct ChangeTrackerTestAccess {
    static void Events(ChangeTracker& tracker, const std::wstring& owner, std::vector<ChangeRecord> events) {
        auto& journal = tracker.journals_[owner];
        uint64_t id = 1;
        for (auto& event : events) { event.id = id++; event.file_id = event.id; event.source = ChangeSource::Event; }
        if (!events.empty()) journal.tracking_since = events.front().time;
        journal.records = std::move(events); ++journal.revision; journal.dirty = true;
    }
    static uint64_t Origin(const ChangeTracker& tracker, const std::wstring& owner) { return tracker.journals_.at(owner).tracking_since; }
    static void GapAt(ChangeTracker& tracker, const std::wstring& owner, uint64_t end) {
        auto& journal = tracker.journals_[owner]; journal.gap = true; journal.gap_end = end;
        journal.gap_until = end + 7 * 86400; journal.dirty = true;
    }
    static void ContinuousWeek(ChangeTracker& tracker, const std::wstring& owner) {
        auto& journal = tracker.journals_[owner]; journal.gap_until = ChangeTracker::Now() - 1;
    }
    static void Expire(ChangeTracker& tracker, const std::wstring& owner) {
        tracker.journals_[owner].expiry = ChangeTracker::Now() - 1;
    }
    static void AgeCleanRecords(ChangeTracker& tracker, const std::wstring& owner) {
        auto& journal = tracker.journals_[owner];
        for (auto& record : journal.records) record.time = ChangeTracker::Now() - 8 * 86400;
        journal.dirty = false;
    }
    static bool Empty(const ChangeTracker& tracker, const std::wstring& owner) {
        return tracker.journals_.at(owner).records.empty();
    }
};
}
int wmain() {
    const auto root = std::filesystem::temp_directory_path() / (L"pulse-changes-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    ChangeTracker tracker; tracker.Open(root.wstring()); tracker.Lease(L"test", true);
    check(tracker.Summaries(L"test", {L"C:\\root"}).state == ChangeState::Available, "first enable starts complete observed history without pre-install gap");
    const auto first_origin = ChangeTrackerTestAccess::Origin(tracker, L"test");
    check(!tracker.Lease(L"test", true) && ChangeTrackerTestAccess::Origin(tracker, L"test") == first_origin &&
        tracker.Summaries(L"test", {L"C:\\root"}).state == ChangeState::Available, "lease renewal preserves origin and never schedules mtime baseline");
    ChangeRecord e; e.path = L"C:\\root\\child\\file.txt"; e.time = ChangeTracker::Now(); e.file_id = 42;
    tracker.Record(e); e.time += 3; tracker.Record(e);
    auto summaries = tracker.Summaries(L"test", {L"C:\\root", L"C:\\root\\child", L"C:\\root2"});
    check(summaries.summaries[0].count == 1 && summaries.summaries[1].count == 1 && summaries.summaries[2].count == 0, "window dedup, ancestor rollup, path boundary");
    e.kind = ChangeKind::Renamed; e.old_path = e.path; e.path = L"C:\\other\\file.txt"; tracker.Record(e);
    auto old = tracker.Details(L"test", L"C:\\root", 0, 0, 200);
    auto moved = tracker.Details(L"test", L"C:\\other", 0, 0, 200);
    check(old.records.front().kind == ChangeKind::MovedOut && moved.records.front().kind == ChangeKind::MovedIn, "move old and new root classification");
    e.kind = ChangeKind::Deleted; e.old_path.clear(); tracker.Record(e);
    check(tracker.Summaries(L"test", {L"C:\\other"}).summaries[0].has_deleted, "deletion summary");
    auto final_state = tracker.Details(L"test", L"C:\\", 0, 0, 200);
    check(final_state.records.size() == 1 && final_state.records.front().kind == ChangeKind::Deleted, "single final outcome per identity uses deletion precedence");
    ChangeRecord second; second.path = L"C:\\other\\second.txt"; tracker.Record(second);
    auto page = tracker.Details(L"test", L"C:\\", 0, 0, 1);
    check(page.records.size() == 1 && page.next_cursor != 0 && !tracker.Details(L"test", L"C:\\", 0, page.next_cursor, 1).records.empty(), "pagination cursor");
    check(tracker.Summaries(L"test", {L"\\\\?\\C:\\other"}).summaries[0].count == 2, "extended path prefix normalization");
    check(tracker.Details(L"other-user", L"C:\\", 0, 0, 200).records.empty(), "owner isolation");
    ChangeRecord initial; initial.path = L"C:\\baseline\\old.txt"; initial.time = ChangeTracker::Now() - 60;
    tracker.Seed(L"test", {initial});
    auto baseline = tracker.Summaries(L"test", {L"C:\\baseline"});
    check(baseline.summaries[0].count == 0 && baseline.summaries[0].initial_count == 0, "pre-start mtime baseline is not imported");
    initial.time = ChangeTracker::Now(); tracker.Record(initial);
    baseline = tracker.Summaries(L"test", {L"C:\\baseline"});
    check(baseline.summaries[0].initial_count == 0 && baseline.summaries[0].counts[1] == 1, "only an observed post-start change enters history");
    tracker.Lease(L"test", false);
    auto prior = tracker.Details(L"test", L"C:\\", 0, 0, 200).records.size();
    e.path = L"C:\\inactive.txt"; tracker.Record(e);
    check(tracker.Details(L"test", L"C:\\", 0, 0, 200).records.size() == prior, "inactive lease ignores events");
    tracker.Flush();
    ChangeTracker reloaded; reloaded.Open(root.wstring()); reloaded.Lease(L"test", true);
    check(reloaded.Details(L"test", L"C:\\", 0, 0, 200).records.size() == prior, "journal reload separate from index delta");
    e.time = ChangeTracker::Now() - 8 * 86400; e.path = L"C:\\historical.txt"; reloaded.Record(e);
    check(reloaded.Details(L"test", L"C:\\", 0, 0, 200).records.size() == prior, "catchup timestamp not reported as current");
    tracker.Lease(L"cutoff", true);
    ChangeRecord a; a.path = L"C:\\cutoff\\a.txt"; a.time = ChangeTracker::Now() - 60;
    ChangeRecord b = a; b.path = L"C:\\cutoff\\b.txt"; b.time += 40;
    ChangeTrackerTestAccess::Events(tracker, L"cutoff", {a, b});
    const auto both = tracker.Summaries(L"cutoff", {L"C:\\cutoff"}, a.time);
    const auto one = tracker.Summaries(L"cutoff", {L"C:\\cutoff"}, a.time + 1);
    check(both.summaries[0].count == 2 && one.summaries[0].count == 1, "sliding window expires at exact timestamp boundary");
    tracker.Gap();
    ChangeTrackerTestAccess::ContinuousWeek(tracker, L"cutoff");
    auto covered = tracker.Summaries(L"cutoff", {L"C:\\cutoff"});
    check(covered.state == ChangeState::Available && !covered.summaries[0].incomplete, "continuous seven day coverage clears gap");
    ChangeTrackerTestAccess::Expire(tracker, L"cutoff");
    check(tracker.Summaries(L"cutoff", {L"C:\\cutoff"}).state == ChangeState::Unavailable, "crashed client lease expiry");
    tracker.Lease(L"cutoff", true);
    check(tracker.Summaries(L"cutoff", {L"C:\\cutoff"}).state == ChangeState::Gap, "lease resume records coverage gap");
    tracker.Lease(L"rename", true);
    ChangeRecord renamed; renamed.path = L"C:\\rename\\new.txt"; renamed.old_path = L"C:\\rename\\old.txt";
    renamed.kind = ChangeKind::Renamed; renamed.file_id = 91; tracker.Record(renamed);
    renamed.kind = ChangeKind::Modified; renamed.old_path.clear(); tracker.Record(renamed);
    auto renamed_result = tracker.Details(L"rename", L"C:\\rename", 0, 0, 200);
    check(renamed_result.records.size() == 1 && renamed_result.records[0].kind == ChangeKind::Renamed &&
        renamed_result.records[0].old_path == L"C:\\rename\\old.txt", "rename then modify preserves source path");
    renamed.kind = ChangeKind::Renamed; renamed.old_path = renamed.path; renamed.path = L"C:\\destination\\new.txt"; tracker.Record(renamed);
    renamed.kind = ChangeKind::Modified; renamed.old_path.clear(); tracker.Record(renamed);
    auto source_result = tracker.Details(L"rename", L"C:\\rename", 0, 0, 200);
    auto destination_result = tracker.Details(L"rename", L"C:\\destination", 0, 0, 200);
    check(source_result.records[0].kind == ChangeKind::MovedOut && !source_result.records[0].old_path.empty() &&
        destination_result.records[0].kind == ChangeKind::MovedIn && !destination_result.records[0].old_path.empty(), "move then modify retains source and destination classification");
    auto common_ancestor = tracker.Summaries(L"rename", {L"C:\\"});
    check(common_ancestor.summaries[0].count == 1 && common_ancestor.summaries[0].counts[static_cast<uint32_t>(ChangeKind::MovedIn)] == 1,
        "cross-parent move appears once at common ancestor as move");
    check(IsChangeJournalName(L"C:\\index\\changes-S-1-5-1.bin") && IsChangeJournalName(L"changes-S-1-5-1.bin.tmp") &&
        !IsChangeJournalName(L"changes-report.txt"), "journal artifact excludes persistent and temporary files");
    tracker.Lease(L"performance", true);
    std::vector<ChangeRecord> fixtures;
    for (uint32_t i = 0; i < 100000; ++i) {
        ChangeRecord entry; entry.time = ChangeTracker::Now() - 20;
        entry.path = L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring(i % 128) + L"\\item" + std::to_wstring(i) + L".txt";
        fixtures.push_back(std::move(entry));
    }
    ChangeTrackerTestAccess::Events(tracker, L"performance", std::move(fixtures));
    std::vector<std::wstring> query_paths;
    for (uint32_t i = 0; i < 128; ++i) query_paths.push_back(L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring(i));
    const auto cutoff = ChangeTracker::Now() - 86400;
    auto start = std::chrono::steady_clock::now();
    const auto cold = tracker.Summaries(L"performance", query_paths, cutoff);
    auto warmed = std::chrono::steady_clock::now();
    const auto warm = tracker.Summaries(L"performance", query_paths, cutoff + 1);
    auto done = std::chrono::steady_clock::now();
    std::printf("[INFO] 100000 observed records / 128 folders: cold %.2f ms, warm sliding %.2f ms\n",
        std::chrono::duration<double, std::milli>(warmed - start).count(), std::chrono::duration<double, std::milli>(done - warmed).count());
    check(cold.summaries.size() == 128 && warm.summaries[0].count == cold.summaries[0].count, "batched cached summaries preserve sliding window results");
    tracker.Lease(L"performance", false);
    ChangeTrackerTestAccess::AgeCleanRecords(tracker, L"performance");
    tracker.Flush();
    check(ChangeTrackerTestAccess::Empty(tracker, L"performance"), "disabled clean journal expires during background flush");
    tracker.Lease(L"normal-cycle", true);
    ChangeRecord cycle; cycle.path = L"C:\\cycle\\file.txt"; tracker.Record(cycle);
    const auto normal_origin = ChangeTrackerTestAccess::Origin(tracker, L"normal-cycle");
    tracker.Lease(L"normal-cycle", false); tracker.Flush();
    check(tracker.Summaries(L"normal-cycle", {L"C:\\cycle"}).state == ChangeState::Unavailable, "normal full exit reports observation offline");
    ChangeTracker normal_reload; normal_reload.Open(root.wstring()); normal_reload.Lease(L"normal-cycle", true);
    check(normal_reload.Summaries(L"normal-cycle", {L"C:\\cycle"}).state == ChangeState::Gap &&
        ChangeTrackerTestAccess::Origin(normal_reload, L"normal-cycle") == normal_origin &&
        normal_reload.Details(L"normal-cycle", L"C:\\cycle", 0, 0, 200).records.size() == 1,
        "clean restart preserves origin and events while marking uncaptured offline interval");
    tracker.Lease(L"active-cycle", true); tracker.Flush();
    ChangeTracker crash_reload; crash_reload.Open(root.wstring()); crash_reload.Lease(L"active-cycle", true);
    check(crash_reload.Summaries(L"active-cycle", {L"C:\\"}).state == ChangeState::Gap, "persisted active lease detects unclean restart");
    tracker.Lease(L"scope-gap", true);
    ChangeTrackerTestAccess::GapAt(tracker, L"scope-gap", ChangeTracker::Now());
    check(tracker.Summaries(L"scope-gap", {L"C:\\"}).state == ChangeState::Gap &&
        tracker.Summaries(L"scope-gap", {L"C:\\"}, ChangeTracker::Now() + 1).state == ChangeState::Available,
        "gap affects only query windows that overlap its end");
    auto write_legacy = [&](const std::wstring& owner, std::vector<ChangeRecord> records) {
        pulse::ipc::PayloadWriter writer; writer.PutU32(0x32484350); writer.PutU32(static_cast<uint32_t>(records.size()));
        uint64_t id = 1;
        for (const auto& record : records) {
            writer.PutU64(id++); writer.PutU64(record.time); writer.PutU64(record.file_id);
            writer.PutU32(static_cast<uint32_t>(record.kind)); writer.PutU32(record.is_dir ? 1u : 0u);
            writer.PutU32(static_cast<uint32_t>(record.source)); writer.PutString(record.path); writer.PutString(record.old_path);
        }
        std::ofstream output(root / (L"changes-" + owner + L".bin"), std::ios::binary);
        output.write(reinterpret_cast<const char*>(writer.data().data()), static_cast<std::streamsize>(writer.data().size()));
    };
    ChangeRecord legacy_event; legacy_event.path = L"C:\\legacy\\captured.txt"; legacy_event.time = ChangeTracker::Now() - 60;
    ChangeRecord legacy_initial = legacy_event; legacy_initial.path = L"C:\\legacy\\mtime.txt"; legacy_initial.source = ChangeSource::InitialMtime;
    write_legacy(L"legacy", {legacy_event, legacy_initial});
    ChangeTracker legacy; legacy.Open(root.wstring()); legacy.Lease(L"legacy", true);
    const auto legacy_result = legacy.Details(L"legacy", L"C:\\legacy", 0, 0, 200);
    check(legacy_result.records.size() == 1 && legacy_result.records[0].path == legacy_event.path &&
        legacy_result.state == ChangeState::Gap && ChangeTrackerTestAccess::Origin(legacy, L"legacy") == legacy_event.time,
        "v2 upgrade preserves actual events, removes mtime baseline and does not claim restart continuity");
    write_legacy(L"legacy-initial", {legacy_initial});
    ChangeTracker legacy_initial_only; legacy_initial_only.Open(root.wstring()); legacy_initial_only.Lease(L"legacy-initial", true);
    check(legacy_initial_only.Summaries(L"legacy-initial", {L"C:\\legacy"}).state == ChangeState::Available &&
        legacy_initial_only.Details(L"legacy-initial", L"C:\\legacy", 0, 0, 200).records.empty(),
        "v2 mtime-only history starts fresh without invented interruption");
    legacy.Lease(L"legacy", false); legacy.Flush();
    ChangeTracker upgraded; upgraded.Open(root.wstring()); upgraded.Lease(L"legacy", true);
    check(ChangeTrackerTestAccess::Origin(upgraded, L"legacy") == legacy_event.time &&
        upgraded.Details(L"legacy", L"C:\\legacy", 0, 0, 200).records.size() == 1, "v3 origin survives migration rewrite and reload");
    { std::ofstream broken(root / L"changes-corrupt.bin", std::ios::binary); broken << "bad"; }
    ChangeTracker broken; broken.Open(root.wstring()); broken.Lease(L"corrupt", true);
    check(broken.Summaries(L"corrupt", {L"C:\\"}).state == ChangeState::Gap, "corrupt existing journal is not silently treated as fresh history");
    ChangeTracker recycle; recycle.Open(root.wstring()); recycle.Lease(L"recycle", true);
    ChangeRecord bin; bin.file_id = 777; bin.kind = ChangeKind::Renamed;
    bin.old_path = L"C:\\docs\\report.txt";
    bin.path = L"\\\\?\\C:\\$Recycle.Bin\\S-1-5-21-123\\$RABC.txt";
    recycle.Record(bin);
    auto removed = recycle.Details(L"recycle", L"C:\\docs", 0, 0, 200);
    check(removed.records.size() == 1 && removed.records[0].kind == ChangeKind::Deleted &&
        removed.records[0].path == bin.old_path && removed.records[0].old_path.empty() && removed.records[0].file_id == 777,
        "recycle rename becomes original path deletion");
    check(recycle.Summaries(L"recycle", {L"C:\\docs"}).summaries[0].has_deleted &&
        recycle.Details(L"recycle", L"C:\\$Recycle.Bin", 0, 0, 200).records.empty(), "deletion minus without recycle garbage rows");
    std::swap(bin.path, bin.old_path); recycle.Record(bin);
    check(recycle.Details(L"recycle", L"C:\\docs", 0, 0, 200).records[0].kind == ChangeKind::Created &&
        !recycle.Summaries(L"recycle", {L"C:\\docs"}).summaries[0].has_deleted, "restore supersedes deletion");
    std::swap(bin.path, bin.old_path); recycle.Record(bin);
    check(recycle.Summaries(L"recycle", {L"C:\\docs"}).summaries[0].has_deleted, "rapid redelete is not discarded as duplicate");
    bin.file_id = 778; bin.old_path = L"C:\\docs\\ordinary.txt";
    bin.path = L"C:\\archive\\$Recycle.Bin\\S-1-5-21-123\\ordinary.txt"; recycle.Record(bin);
    check(recycle.Details(L"recycle", L"C:\\archive", 0, 0, 200).records[0].kind == ChangeKind::MovedIn,
        "nested recycle-named directory stays ordinary move");
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
