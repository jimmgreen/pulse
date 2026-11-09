#include "../app/group_selection_plan.h"
#include "../app/context_menu.h"
#include "../app/create_rename_intent.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>

namespace filesystem = std::filesystem;
using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
std::string Contents(const filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto root = filesystem::absolute(L"bench_data") / (L"group-selection-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    if (filesystem::exists(root) || !filesystem::create_directories(root)) return 2;
    try {
        const auto child = root / L"expanded" / L"child";
        filesystem::create_directories(child / L"Grouped");
        filesystem::create_directories(root / L"Grouped");
        std::ofstream(child / L"first.txt") << "first payload";
        std::ofstream(child / L"second.txt") << "second payload";
        std::ofstream(child / L"Grouped" / L"keep.txt") << "child conflict";
        std::ofstream(root / L"Grouped" / L"keep.txt") << "upper conflict";
        const std::vector<std::wstring> selected{(child / L"first.txt").wstring(), (child / L"second.txt").wstring()};
        std::wstring resolver_parent;
        const auto plan = app::PlanGroupSelection(selected, [&](const std::wstring& parent) {
            resolver_parent = parent;
            return app::UniqueChildName(parent, L"Grouped", L"");
        });
        Check(plan && plan->parent == child.wstring() && resolver_parent == child.wstring() &&
            plan->destination == (child / L"Grouped (2)").wstring(),
            "expanded child sources choose their own parent and resolve its existing folder conflict");
        if (!plan) throw std::runtime_error("missing grouping plan");
        const auto old_destination = root / app::UniqueChildName(root.wstring(), L"Grouped", L"");
        Check(old_destination != filesystem::path(plan->destination), "tab-root destination reproduces the reported wrong-parent planning defect");
        Check(CreateDirectoryW(plan->destination.c_str(), nullptr) != FALSE, "planned child destination is created");
        bool moved = true;
        for (const auto& source : plan->sources) {
            const auto destination = filesystem::path(plan->destination) / filesystem::path(source).filename();
            moved = MoveFileW(source.c_str(), destination.c_str()) != FALSE && moved;
        }
        Check(moved && Contents(filesystem::path(plan->destination) / L"first.txt") == "first payload" &&
            Contents(filesystem::path(plan->destination) / L"second.txt") == "second payload" &&
            !filesystem::exists(child / L"first.txt") && !filesystem::exists(child / L"second.txt") && !filesystem::exists(old_destination),
            "real selected files land under the expanded parent, never the displayed upper folder");
        Check(Contents(child / L"Grouped" / L"keep.txt") == "child conflict" &&
            Contents(root / L"Grouped" / L"keep.txt") == "upper conflict", "existing folders and their contents remain untouched");

        filesystem::create_directories(root / L"other");
        std::ofstream(child / L"remain.txt") << "child remaining";
        std::ofstream(root / L"other" / L"remain.txt") << "other remaining";
        const std::vector<std::wstring> mixed{(child / L"remain.txt").wstring(), (root / L"other" / L"remain.txt").wstring()};
        bool resolved = false;
        const auto refused = app::PlanGroupSelection(mixed, [&](const auto&) { resolved = true; return L"Unexpected"; });
        Check(!refused && !resolved && Contents(child / L"remain.txt") == "child remaining" &&
            Contents(root / L"other" / L"remain.txt") == "other remaining", "different-parent selection is refused without naming or moving anything");
        const std::vector<std::wstring> top{(root / L"one").wstring(), (root / L"two").wstring()};
        Check(app::GroupSelectionParent(top) == root.wstring(), "ordinary same-folder selection retains its destination parent");
        const std::vector<std::wstring> unc{L"\\\\server\\share\\deep\\one", L"\\\\server\\share\\deep\\two"};
        Check(app::GroupSelectionParent(unc) == L"\\\\server\\share\\deep", "UNC child selection retains its share and depth without network IO");
        const std::vector<std::wstring> extended{L"\\\\?\\C:\\deep\\one", L"C:\\deep\\two"};
        Check(app::GroupSelectionParent(extended) == L"\\\\?\\C:\\deep", "extended-prefix spelling preserves native destination path");
        const std::vector<std::wstring> case_distinct{L"C:\\Foo\\one", L"C:\\foo\\two"};
        Check(!app::GroupSelectionParent(case_distinct), "case-distinct parents are not silently merged");

        std::vector<app::CreateRenameIntent> intents;
        app::QueueCreateRenameIntent(intents, 42, root.wstring(), 7, plan->parent);
        Check(app::CompleteCreateRenameIntent(intents, 42, root.wstring(), 7, plan->destination),
            "nested create completion stays bound to the original displayed root");
        const auto name = filesystem::path(plan->destination).filename().wstring();
        const auto wrong_path = (root / name).wstring();
        const app::CreateRenameEntry rows[] = {{name, {}}, {name, wrong_path}, {name, plan->destination}};
        auto rename = app::SelectCreateRenameIntent(intents, root.wstring(), 7, rows, true, false);
        Check(rename && rename->entry_index == 2 && rename->task_id == 42,
            "automatic rename selects the full child path, not an upper row with the same name");
        Check(!app::SelectCreateRenameIntent(intents, root.wstring(), 7, rows, false, false) &&
            !app::SelectCreateRenameIntent(intents, root.wstring(), 7, rows, true, true),
            "nested rename waits while another pane or editor owns focus");
        Check(!app::SelectCreateRenameIntent(intents, (root / L"other").wstring(), 7, rows, true, false) && intents.empty(),
            "navigation cancels delayed nested automatic rename");
    } catch (const std::exception& error) { std::printf("[FAIL] %s\n", error.what()); ++failures; }
    // Only this exclusively created fixture root is removed.
    std::error_code error;
    filesystem::remove_all(root, error);
    Check(!error && !filesystem::exists(root), "isolated grouping fixture cleanup");
    return failures ? 1 : 0;
}
