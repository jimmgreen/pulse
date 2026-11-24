#include "../ops/ops_manager.h"
#include "../ops/interactive_name_reservation.h"
#include "../ops/interactive_tree_watch.h"
#include "../ops/interactive_sibling_listing.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <thread>

using namespace pulse;
using namespace pulse::ops;
namespace fs = std::filesystem;
namespace names = interactive_names;
namespace {
int failures;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void NameCases() {
    using F = names::Family;
    const auto transfer = names::Make(L"C:\\out\\tree.jpg", F::Transfer);
    auto test = [&](const std::wstring& candidate, bool expect, const char* label) {
        const auto other = names::Make(candidate, F::NumberedDirectory);
        Check(names::Overlaps(transfer, other) == expect && names::Overlaps(other, transfer) == expect, label);
    };
    test(L"C:\\out\\TREE.jpg\\child", true, "exact target, case insensitive, with descendant");
    test(L"C:\\out", true, "ancestor of destination");
    test(L"C:\\out\\treehouse.jpg", false, "same parent and textual prefix only");
    test(L"C:\\out\\tree.png", false, "same stem, different extension");
    const auto pid = std::to_wstring(GetCurrentProcessId());
    test(L"C:\\out\\tree.jpg.pulse-backup-" + pid + L"-42-0", true, "replacement backup");
    test(L"C:\\out\\tree.jpg.pulse-backup-note", false, "ordinary name beginning like a backup");
    for (auto lang : {l10n::Language::EnUS, l10n::Language::ZhCN, l10n::Language::ZhTW}) {
        const auto suffix = l10n::GetForLanguage(l10n::StringId::Copy_2, lang);
        Check(!suffix.empty(), "catalog supplies localized copy suffix");
        test(L"C:\\out\\tree" + suffix + L" (8).jpg", true, "localized numbered copy");
        test(L"C:\\out\\tree" + suffix + L" (8).jpg.pulse-verify-" + pid + L"-3-0-4", true,
            "verification scratch of a localized numbered copy");
        test(L"C:\\out\\tree" + suffix + L" (8)" + suffix + L".jpg.pulse-copy-" + pid + L"-4-0", true,
            "scratch of name generated once in self-copy planning and again during execution");
        test(L"C:\\out\\tree.jpg" + suffix + L"\\child", true, "dotted directory copy descendant");
    }
    auto renamed = names::Make(L"C:\\out\\my.folder", F::NumberedUnknown);
    Check(names::Overlaps(renamed, names::Make(L"C:\\out\\my.folder (2)\\a")), "numbered rename of dotted directory");
    Check(names::Overlaps(renamed, names::Make(L"C:\\out\\my (2).folder")), "numbered file rename");
    Check(!names::Overlaps(renamed, names::Make(L"C:\\out\\my (two).folder")), "non-numbered sibling remains independent");
    std::vector<names::Root> left{names::Make(L"X:\\out\\abc"), names::Make(L"X:\\out\\def")};
    std::vector<names::Root> right{names::Make(L"Y:\\out\\abc\\child")};
    auto unknown = right;
    unsigned queries = 0;
    Check(names::NormalizeVolumes(left, right, [&](wchar_t, std::wstring& out) {
        ++queries; out = L"volume-one\\"; return true;
    }) && queries == 2 && names::Overlaps(left.front(), right.front()),
        "two letters resolving to one volume compare as aliases, one lookup per letter");
    left = {names::Make(L"X:\\out\\abc")};
    Check(!names::NormalizeVolumes(left, unknown, [](wchar_t, std::wstring&) { return false; }),
        "unknown volume identity cannot admit mixed drives");
}
auto Deadline(unsigned ms) { return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); }
template<class F> bool Until(F fn, unsigned ms) {
    const auto deadline = Deadline(ms);
    do {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return fn();
}
void Put(const fs::path& p, const char* contents) { std::ofstream(p, std::ios::binary) << contents; }
std::string Read(const fs::path& p) { std::string s; std::ifstream(p, std::ios::binary) >> s; return s; }
void WatchCases(const fs::path& base) {
    const auto root = fs::absolute(base) / (L"watch-" + std::to_wstring(GetTickCount64()));
    const auto watched = root / "watched", other = root / "other", file = watched / "sub" / "file";
    fs::create_directories(watched / "sub"); fs::create_directories(other);
    InteractiveTreeWatch watch;
    const auto stable = [&] { return watch.Unchanged(41, watched.wstring()); };
    watch.Arm(41, watched.wstring());
    Check(stable() && !watch.Unchanged(42, watched.wstring()) && !watch.Unchanged(41, other.wstring()),
        "tree proof belongs to one task and one directory identity");
    Put(file, "watched");
    Check(Until([&] { return !stable(); }, 3000), "creating a real nested file invalidates the watch");
    watch.Arm(41, watched.wstring());
    const bool armed_attr = stable();
    Check(armed_attr && SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_HIDDEN) &&
        Until([&] { return !stable(); }, 3000), "changing a nested file attribute invalidates the watch");
    watch.Arm(41, watched.wstring());
    const bool armed_delete = stable();
    Check(armed_delete && fs::remove(file) && Until([&] { return !stable(); }, 3000),
        "removing a real nested file invalidates the watch");
    watch.Arm(41, watched.wstring());
    const bool armed_rename = stable();
    fs::rename(watched, root / "original"); fs::create_directory(watched);
    Check(armed_rename && !stable(), "replacing the watched root path rejects the formerly open identity");
    watch.Reset();
    Check(!stable(), "reset removes the positive tree proof");
    std::vector<std::wstring> leaves;
    for (int i = 0; i < 24; ++i) leaves.push_back(L"item" + std::to_wstring(i));
    Put(watched / "item0", "file"); fs::create_directory(watched / "item1");
    InteractiveSiblingListing listing;
    leaves[1] = L"ITEM1";
    Check(listing.Read(watched.wstring(), leaves, GetTickCount64() + 3000) &&
        listing.Get(leaves[0]) == InteractiveSiblingListing::Kind::PlainFile &&
        listing.Get(leaves[1]) == InteractiveSiblingListing::Kind::Directory &&
        listing.Get(leaves[2]) == InteractiveSiblingListing::Kind::Absent, "one bounded snapshot separates ordinal file, directory and missing sibling");
    Check(!listing.Read(watched.wstring(), leaves, 0), "expired or incomplete enumeration never proves an absent sibling");
    leaves.back() = L"LONGFI~1";
    Check(!listing.Read(watched.wstring(), leaves, GetTickCount64() + 3000), "possible 8.3 alias forces conservative path");
}
void SwitchPrimary(const fs::path& base) {
    const auto root = fs::absolute(base) / (L"switch-" + std::to_wstring(GetTickCount64()));
    const auto src = root / "src", dst = root / "dst";
    for (auto tree : {L"tree-a", L"tree-b"}) {
        fs::create_directories(src / tree); fs::create_directories(dst / tree);
        Put(src / tree / "collision", "new"); Put(dst / tree / "collision", "old");
    }
    OpsManager manager; manager.Start([] {}, false);
    const auto copy = [&](const wchar_t* name) {
        OpRequest q; q.type = OpType::Copy; q.sources = {(src / name).wstring()}; q.dest_dir = dst.wstring();
        return manager.Submit(std::move(q));
    };
    const auto create = [&](const fs::path& path) {
        OpRequest q; q.type = OpType::CreateFolder; q.sources = {path.wstring()}; q.collision_policy = CollisionPolicy::KeepBoth;
        return manager.Submit(std::move(q));
    };
    auto gate = manager.PendingConflict();
    const auto gated = [&](uint64_t task) {
        return Until([&] { gate = manager.PendingConflict(); return gate && gate->task_id == task; }, 15000);
    };
    const auto first = copy(L"tree-a");
    if (!gated(first)) { Check(false, "first transition conflict"); manager.CancelCurrent(); manager.Stop(); return; }
    const auto second = copy(L"tree-b");
    const auto dependent = src / "tree-a" / "added";
    const auto originally_blocked = create(dependent), witness = create(root / "witness");
    Check(Until([&] { return manager.FinalStatus(witness).has_value(); }, 8000) && !fs::exists(dependent) &&
        !manager.FinalStatus(originally_blocked), "scheduler passed the blocked candidate while first primary remained gated");
    manager.ResolveConflict(gate->token, ConflictChoice::Replace, true);
    if (!gated(second)) { Check(false, "second transition conflict"); manager.CancelCurrent(); manager.Stop(); return; }
    Check(Until([&] { return manager.FinalStatus(originally_blocked).has_value(); }, 8000) &&
        fs::is_directory(dependent) && !manager.FinalStatus(second), "negative admission for first primary does not pin candidate behind second");
    manager.ResolveConflict(gate->token, ConflictChoice::Replace, true);
    Check(Until([&] { return manager.FinalStatus(second).has_value(); }, 30000) &&
        Read(dst / "tree-a" / "collision") == "new" && Read(dst / "tree-b" / "collision") == "new",
        "both conflict-gated bulk tasks completed");
    manager.Stop();
}
void SameObjectCopy(const fs::path& base) {
    const auto root = fs::absolute(base) / (L"same-object-" + std::to_wstring(GetTickCount64()));
    const auto dst = root / "out", a = root / "a" / "sample.txt", b = root / "b" / "sample.txt";
    fs::create_directories(a.parent_path()); fs::create_directories(b.parent_path()); fs::create_directories(dst);
    Put(dst / "sample.txt", "original");
    fs::create_hard_link(dst / "sample.txt", a); fs::create_hard_link(dst / "sample.txt", b);
    const auto suffix = l10n::Get(l10n::StringId::Copy_2);
    const auto first = dst / (L"sample" + suffix + L".txt"), second = dst / (L"sample" + suffix + suffix + L".txt");
    OpsManager manager; manager.Start([] {}, false);
    OpRequest copy; copy.type = OpType::Copy; copy.sources = {a.wstring(), b.wstring()}; copy.dest_dir = dst.wstring();
    const auto primary = manager.Submit(std::move(copy));
    auto gate = manager.PendingConflict();
    if (!Until([&] { gate = manager.PendingConflict(); return gate && gate->task_id == primary; }, 10000)) {
        Check(false, "two different hard-link source paths produce actual self-copy planning collision");
        manager.CancelCurrent(); manager.Stop(); return;
    }
    const auto create = [&](const fs::path& path) {
        OpRequest q; q.type = OpType::CreateFolder; q.sources = {path.wstring()}; q.collision_policy = CollisionPolicy::KeepBoth;
        return manager.Submit(std::move(q));
    };
    const auto reserved = create(second), witness = create(dst / "unrelated");
    Check(Until([&] { return manager.FinalStatus(witness).has_value(); }, 8000) && !manager.FinalStatus(reserved) &&
        !fs::exists(second), "second-generation self-copy name cannot be taken while actual hard-link copy is gated");
    manager.ResolveConflict(gate->token, ConflictChoice::KeepBoth, true);
    Check(Until([&] { return manager.FinalStatus(reserved).has_value(); }, 15000) && Read(first) == "original" &&
        Read(second) == "original" && fs::is_directory(second.wstring() + L" (2)") && Read(a) == "original",
        "both hard-link copies keep the planned content and later create receives the numbered folder");
    manager.Stop();
}
void RunOperations(const fs::path& base) {
    const auto root = fs::absolute(base) / (L"fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto src = root / "in" / "tree", dst = root / "out", light = root / "light";
    fs::create_directories(src); fs::create_directories(dst); fs::create_directories(light / "before");
    fs::create_directories(light / "keep.dir"); Put(src / "payload", "new"); Put(dst / "tree", "old");
    Put(light / "before" / "marker", "directory"); Put(light / "a.txt", "renamed"); Put(light / "keep.txt", "untouched");
    fs::create_directories(light / "out"); Put(light / "small-copy", "copy"); Put(light / "small-move", "move");
    OpsManager manager;
    manager.Start([] {}, false);
    OpRequest bulk; bulk.type = OpType::Copy; bulk.dest_dir = dst.wstring(); bulk.sources = {src.wstring()};
    const auto primary = manager.Submit(bulk);
    auto gate = manager.PendingConflict();
    if (!Until([&] { gate = manager.PendingConflict(); return gate && gate->task_id == primary; }, 15000)) {
        Check(false, "real file/directory replacement conflict"); manager.CancelCurrent(); manager.Stop(); return;
    }
    auto submit = [&](OpType type, const fs::path& p, std::wstring name = {}) {
        OpRequest q; q.type = type; q.sources = {p.wstring()}; q.new_name = std::move(name);
        q.collision_policy = CollisionPolicy::KeepBoth; return manager.Submit(std::move(q));
    };
    const auto bye = dst / ("tree.pulse-backup-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(primary) + "-0");
    auto treehouse = submit(OpType::CreateFolder, dst / "treehouse");
    auto file = submit(OpType::Rename, light / "a.txt", L"keep.txt");
    auto directory = submit(OpType::Rename, light / "before", L"keep.dir");
    const auto single_transfer = [&](OpType type, const wchar_t* leaf) {
        OpRequest q; q.type = type; q.sources = {(light / leaf).wstring()}; q.dest_dir = (light / "out").wstring();
        q.collision_policy = CollisionPolicy::KeepBoth; return manager.Submit(std::move(q));
    };
    auto copy = single_transfer(OpType::Copy, L"small-copy"), move = single_transfer(OpType::Move, L"small-move");
    {
        std::ofstream large(light / "large-template", std::ios::binary);
        large << std::string(1024 * 1024 + 1, 'x');
    }
    const auto from_template = [&](const fs::path& source, const char* leaf) {
        OpRequest q; q.type = OpType::CreateTextFile; q.sources = {(light / leaf).wstring()};
        q.template_file = source.wstring(); q.collision_policy = CollisionPolicy::KeepBoth;
        return manager.Submit(std::move(q));
    };
    auto light_template = from_template(light / "keep.txt", "independent-template");
    // Reads what the held copy only reads: runs at once. Reads what it replaces: waits.
    auto related_template = from_template(src / "payload", "related-template");
    auto dependent_template = from_template(dst / "tree", "dependent-template");
    auto large_template = from_template(light / "large-template", "large-result");
    auto backup = submit(OpType::CreateFolder, bye);
    auto localized = submit(OpType::CreateFolder, dst / (L"tree" + l10n::Get(l10n::StringId::Copy_2)));
    auto outer = submit(OpType::Rename, dst, L"moved-out");
    Check(Until([&] { return manager.FinalStatus(treehouse) && manager.FinalStatus(file) && manager.FinalStatus(directory) &&
        manager.FinalStatus(copy) && manager.FinalStatus(move) && manager.FinalStatus(light_template) &&
        manager.FinalStatus(related_template); }, 8000),
        "independent create and UI-policy collision renames finish while copy is held");
    Check(Read(light / "out" / "small-copy") == "copy" && Read(light / "small-copy") == "copy" &&
        Read(light / "out" / "small-move") == "move" && !fs::exists(light / "small-move"), "small KeepBoth copy and move finish before release");
    Check(Read(light / "independent-template") == "untouched" && Read(light / "related-template") == "new" &&
        !manager.FinalStatus(dependent_template) && !fs::exists(light / "dependent-template") &&
        !manager.FinalStatus(large_template) && !fs::exists(light / "large-result"),
        "independent and shared-read ShellNew templates publish real bytes; a template the copy replaces and an oversized one wait");
    Check(fs::is_directory(dst / "treehouse") && Read(light / "keep (2).txt") == "renamed" &&
        Read(light / "keep.txt") == "untouched" && Read(light / "keep.dir (2)" / "marker") == "directory" &&
        fs::is_directory(light / "keep.dir"), "actual numbered names preserve original occupants");
    Check(!manager.FinalStatus(primary) && !manager.FinalStatus(backup) && !fs::exists(bye) &&
        !manager.FinalStatus(localized) && !manager.FinalStatus(outer) && !fs::exists(root/"moved-out"),
        "backup, localized alternate and enclosing directory wait for held replacement");
    manager.ResolveConflict(gate->token, ConflictChoice::Replace, true);
    bool finished = Until([&] { return manager.FinalStatus(outer).has_value(); }, 30000);
    if (!finished) manager.CancelCurrent();
    const auto moved = root / "moved-out";
    const auto outer_status = manager.FinalStatus(outer);
    std::printf("outer task=%llu terminal=%d phase=%d error=%ls old=%d new=%d backup=%d alternate=%d treehouse=%d payload=%s\n",
        outer, finished, outer_status ? static_cast<int>(outer_status->phase) : -1,
        outer_status ? outer_status->last_error.c_str() : L"no final",
        fs::exists(dst), fs::exists(moved), fs::is_directory(moved / bye.filename()),
        fs::is_directory(moved / (L"tree" + l10n::Get(l10n::StringId::Copy_2))),
        fs::is_directory(moved / "treehouse"), Read(moved / "tree" / "payload").c_str());
    Check(finished && fs::is_directory(moved / "treehouse") && fs::is_directory(moved / bye.filename()) &&
        fs::is_directory(moved / (L"tree" + l10n::Get(l10n::StringId::Copy_2))) &&
        Read(moved / "tree" / "payload") == "new" && !fs::exists(dst),
        "actual replacement completed before reserved siblings and enclosing rename");
    Check(Read(light / "related-template") == "new" && Read(light / "large-result") == std::string(1024 * 1024 + 1, 'x'),
        "serialized ShellNew templates preserve all source content");
    Check(manager.FinalStatus(dependent_template).has_value(), "the template the copy replaced is used only after it");
    manager.Stop();
    std::printf("fixture=%ls\n", root.c_str());
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    NameCases(); WatchCases(argv[1]); RunOperations(argv[1]); SwitchPrimary(argv[1]); SameObjectCopy(argv[1]);
    CoUninitialize(); std::printf("failures=%d\n", failures); return failures ? 1 : 0;
}
