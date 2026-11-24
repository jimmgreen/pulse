#include "../app/git_row_lookup.h"
#include "../ui/ui_view_model.h"
#include <iostream>
using namespace pulse;
int main() {
    int failures = 0, passes = 0;
    const auto check = [&](bool ok, const char* text) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n'; ok ? ++passes : ++failures; };
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 30000; ++i) { fs::DirEntry e; e.name = L"f" + std::to_wstring(i); entries->push_back(std::move(e)); }
    auto status = std::make_shared<app::GitRepoStatus>();
    status->files[L"sub\\f0"] = ui::GitMark::Modified;
    status->files[L"sub\\f29999"] = ui::GitMark::Added;
    auto lookup = app::MakeGitRowLookup(entries, status, L"C:\\repo\\sub", L"C:\\repo");
    bool same = true;
    for (int i = 0; i < 30000; ++i)
        same &= lookup(i) == app::GitMarkFor(*status, L"sub\\" + app::GitLowerPath((*entries)[i].name), false);
    check(same, "all 30000 ordinary row marks equal former eager evaluation");
    check(lookup(29999) == ui::GitMark::Added && lookup(0) == ui::GitMark::Modified, "scroll to distant source preserves marks");
    check(lookup(-1) == ui::GitMark::None && lookup(30000) == ui::GitMark::None, "invalid source indices are ignored");
    auto next = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    std::swap((*next)[0], (*next)[29999]);
    auto sorted = app::MakeGitRowLookup(next, status, L"C:\\repo\\sub", L"C:\\repo");
    check(sorted(0) == ui::GitMark::Added && lookup(0) == ui::GitMark::Modified, "new order keeps old frame immutable");
    auto renamed = std::make_shared<std::vector<fs::DirEntry>>(*next);
    (*renamed)[0].name = L"renamed";
    auto new_status = std::make_shared<app::GitRepoStatus>(*status);
    new_status->files[L"sub\\renamed"] = ui::GitMark::Renamed;
    auto fresh = app::MakeGitRowLookup(renamed, new_status, L"C:\\repo\\sub", L"C:\\repo");
    check(fresh(0) == ui::GitMark::Renamed && sorted(0) == ui::GitMark::Added, "rename and Git answer replace without stale cache");
    auto tree = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry child; child.name = L"f0"; child.full_path = L"\\\\?\\C:\\repo\\sub\\child\\f0"; tree->push_back(child);
    new_status->files[L"sub\\child\\f0"] = ui::GitMark::Conflict;
    auto nested = app::MakeGitRowLookup(tree, new_status, L"\\\\?\\C:\\repo\\sub", L"C:\\repo");
    check(nested(0) == ui::GitMark::Conflict, "expanded child uses full source path and extended prefix");
    entries.reset(); status.reset();
    check(lookup(0) == ui::GitMark::Modified, "closure owns listing and status after caller lifetime");
    ui::PaneViewModel pane;
    auto svn = std::make_shared<std::unordered_map<int, ui::GitMark>>(); (*svn)[0] = ui::GitMark::Locked;
    pane.git_marks = svn;
    check(pane.GitMarkAt(0) == ui::GitMark::Locked, "SVN map fallback unchanged");
    pane.git_mark_lookup = lookup;
    auto copied = pane;
    check(copied.GitMarkAt(0) == ui::GitMark::Modified, "copied pane and hit-test accessor use Git callback");
    check(!app::MakeGitRowLookup(tree, new_status, L"C:\\repository", L"C:\\repo"), "repository prefix boundary rejected");
    auto unc_entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry unc; unc.name = L"f0"; unc_entries->push_back(unc);
    auto unc_lookup = app::MakeGitRowLookup(unc_entries, new_status, LR"(\\server\share\repo\sub)", LR"(\\server\share\repo)");
    check(unc_lookup && unc_lookup(0) == ui::GitMark::Modified, "ordinary UNC repository mapping");
    unc.full_path = LR"(\\?\UNC\server\share\repo\sub\child\f0)";
    auto unc_tree = std::make_shared<std::vector<fs::DirEntry>>(1, unc);
    auto unc_extended = app::MakeGitRowLookup(unc_tree, new_status, LR"(\\?\UNC\server\share\repo\sub)", LR"(\\server\share\repo)");
    check(unc_extended && unc_extended(0) == ui::GitMark::Conflict, "extended UNC expanded child mapping");
    std::cout << "Passes: " << passes << " Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
