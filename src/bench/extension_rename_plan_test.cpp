#include "../app/extensions/rename_plan.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace pulse::extensions;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char *label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
                                                (L"extension-rename-plan-" + std::to_wstring(GetCurrentProcessId()) +
                                                 L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root.parent_path());
    if (!std::filesystem::create_directory(root))
        return 2;
    auto write = [&](const wchar_t *leaf) { std::ofstream(root / leaf) << "isolated rename fixture"; };
    write(L"a.txt");
    write(L"b.txt");
    auto make = [&](std::vector<std::wstring> names) {
        RenameDiskPlan plan;
        plan.directory = root.wstring();
        plan.names = std::move(names);
        for (const auto *name : {L"a.txt", L"b.txt"}) {
            pulse::app::rename::Source source;
            source.name = name;
            source.path = (root / name).wstring();
            plan.sources.push_back(std::move(source));
        }
        return plan;
    };
    const auto allowed = [] { return true; };
    auto plan = make({L"b.txt", L"c.txt"});
    check(ValidateRenameDisk(plan, false, allowed).empty(), "developer supplied name mapping produces a plan");
    check(plan.preview.exec_order == std::vector<size_t>({1, 0}),
          "existing chain-safe planner orders dependent renames");
    check(ValidateRenameDisk(plan, true, allowed).empty(), "unchanged identities and targets revalidate");
    check(std::filesystem::exists(root / L"a.txt") && !std::filesystem::exists(root / L"c.txt"),
          "prepare and revalidate never rename files");
    check(ValidateRenameDisk(plan, true, [] { return false; }) == "cancelled",
          "revoked caller cancels disk preparation");
    auto cycle = make({L"b.txt", L"a.txt"});
    check(ValidateRenameDisk(cycle, false, allowed) == "rename_conflict",
          "swap cycle is blocked rather than silently rewritten");
    check(cycle.identities.empty() && cycle.preview.exec_order.empty(),
          "rejected cycle does not publish an executable prepared plan");
    check(std::filesystem::exists(root / L"a.txt") && std::filesystem::exists(root / L"b.txt") &&
              !std::filesystem::exists(root / L"c.txt"),
          "cycle rejection preserves original disk entries");
    auto invalid = make({L"../escape", L"d.txt"});
    check(ValidateRenameDisk(invalid, false, allowed) == "invalid_name", "non-leaf output is rejected");
    auto reserved = make({L"CON", L"d.txt"});
    check(ValidateRenameDisk(reserved, false, allowed) == "invalid_name", "reserved device name is rejected");
    auto duplicate = make({L"x.txt", L"X.txt"});
    check(ValidateRenameDisk(duplicate, false, allowed) == "rename_conflict",
          "case-insensitive duplicate targets are blocked");
    write(L"c.txt");
    check(ValidateRenameDisk(plan, true, allowed) == "rename_conflict",
          "new conflicting destination invalidates prepared plan");
    std::filesystem::remove(root / L"c.txt");
    std::filesystem::rename(root / L"a.txt", root / L"original-a.txt");
    write(L"a.txt");
    check(ValidateRenameDisk(plan, true, allowed) == "stale_reference",
          "replacement source object cannot inherit confirmed identity");
    auto fresh = make({L"x.txt", L"y.txt"});
    check(ValidateRenameDisk(fresh, false, allowed).empty(), "new explicit prepare captures current object identities");
    const auto retired = root.wstring() + L"-retired";
    std::filesystem::rename(root, retired);
    std::filesystem::create_directory(root);
    write(L"a.txt");
    write(L"b.txt");
    check(ValidateRenameDisk(fresh, true, allowed) == "stale_reference",
          "replacement parent and sources invalidate prepared plan");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(retired, error);
    check(!std::filesystem::exists(root) && !std::filesystem::exists(retired), "isolated fixtures are removed");
    return failures ? 1 : 0;
}
