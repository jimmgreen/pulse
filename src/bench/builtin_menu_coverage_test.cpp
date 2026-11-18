#include "../app/context_menu.h"
#include "../app/git_ui.h"
#include "../app/app_model.h"
#include "../index/index_query.h"
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <string_view>

// No preference IO or keyboard customization is needed to build these menus.
// Production builders, command ownership, ordering and filtering remain linked.
namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
std::wstring ShownShortcut(std::wstring_view chord) { return std::wstring(chord); }
}
// These dependencies belong only to palette/search and recent-change builders
// in the same production translation unit. Fail loudly if this suite calls them.
namespace pulse::fs {
bool IsVirtualPath(const std::wstring&) { std::abort(); }
std::wstring NormalizePath(std::wstring) { std::abort(); }
}
namespace pulse::index {
CompiledQuery ParseQuery(std::wstring_view) { std::abort(); }
bool QueryHasContent(const CompiledQuery&) { std::abort(); }
size_t ContentResultStore::Count() const { std::abort(); }
}
namespace pulse::app {
fs::DirEntry Tab::EntryAt(size_t) const { std::abort(); }
std::vector<int> Tab::SelectedIndices() const { std::abort(); }
}
namespace {
using namespace pulse;
using namespace pulse::app;
using Menu = std::vector<ui::FluentMenuItem>;
ui::FluentMenuItem Row(int command) {
    ui::FluentMenuItem row; row.command = command; row.text = L"fixture"; return row;
}
bool Has(const Menu& rows, int command) {
    for (const auto& row : rows) {
        if (row.command == command) return true;
        for (const auto& button : row.quick_swatches) if (button.command == command) return true;
        if (Has(row.children, command)) return true;
    }
    return false;
}
bool WellFormed(const Menu& rows) {
    if (!rows.empty() && rows.back().separator_after) return false;
    for (const auto& row : rows) {
        if (row.command == CmdNone && row.children.empty() && row.quick_swatches.empty()) return false;
        if (!WellFormed(row.children)) return false;
    }
    return true;
}
ui::FluentMenuItem NewFlyout() {
    auto row = Row(CmdNone); row.glyph = L"\xE710"; row.children = BuildNewMenu();
    row.children.push_back(Row(CmdShellNewBase)); return row;
}
ui::FluentMenuItem GitFlyout() {
    auto row = Row(CmdNone); row.glyph = L"\xE8B7";
    row.children = {Row(kCmdGitPull), Row(kCmdGitPush), Row(kCmdGitRefresh)}; return row;
}
}
int main() {
    int failures = 0, checks = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks; failures += !value; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    };
    struct Mapping { int command; BuiltinMenuItem owner; };
    const Mapping added[] = {
        {CmdOpenPath, BuiltinMenuItem::OpenPath}, {CmdGroupIntoFolder, BuiltinMenuItem::GroupIntoFolder},
        {CmdDissolveFolder, BuiltinMenuItem::DissolveFolder}, {CmdSetAlias, BuiltinMenuItem::SetAlias},
        {CmdStageToTray, BuiltinMenuItem::StageToTray}, {CmdCopyToTarget, BuiltinMenuItem::CopyToTarget},
        {CmdMoveToTarget, BuiltinMenuItem::MoveToTarget}, {CmdBatchRename, BuiltinMenuItem::BatchRename},
        {CmdCompareSideBySide, BuiltinMenuItem::CompareSideBySide}, {CmdCloudKeep, BuiltinMenuItem::CloudKeep},
        {CmdCloudFree, BuiltinMenuItem::CloudFree}, {CmdEjectDrive, BuiltinMenuItem::EjectDrive},
        {CmdDisconnectDrive, BuiltinMenuItem::DisconnectDrive}, {CmdUnlockDrive, BuiltinMenuItem::UnlockDrive},
        {CmdMapNetworkDrive, BuiltinMenuItem::MapNetworkDrive}, {CmdForgetNetworkRecent, BuiltinMenuItem::ForgetNetworkRecent},
        {CmdDupKeepThis, BuiltinMenuItem::DupKeepThis}, {CmdDupReapplyRules, BuiltinMenuItem::DupReapplyRules},
        {CmdFindDuplicates, BuiltinMenuItem::FindDuplicates}, {CmdAiOrganizeHere, BuiltinMenuItem::AiOrganizeHere},
        {CmdConnectServer, BuiltinMenuItem::ConnectServer}, {CmdUnpinNetwork, BuiltinMenuItem::PinNetwork}
    };
    for (const auto& mapping : added) {
        ContextMenuPrefs prefs; prefs.persist = false;
        prefs.SetBuiltinVisible(mapping.owner, false);
        Menu rows{Row(CmdOpen), Row(mapping.command)};
        ApplyBuiltinMenuPrefs(rows, prefs);
        bool ok = BuiltinItemForCommand(mapping.command) == mapping.owner && !Has(rows, mapping.command) && Has(rows, CmdOpen);
        prefs.SetBuiltinVisible(mapping.owner, true);
        rows = {Row(mapping.command)}; ApplyBuiltinMenuPrefs(rows, prefs);
        ok &= Has(rows, mapping.command);
        char label[100]{}; std::snprintf(label, sizeof(label), "added command %d follows its toggle in both directions", mapping.command);
        check(ok, label);
    }
    ContextMenuPrefs prefs; prefs.persist = false;
    for (auto surface : {BuiltinMenuSurface::Item, BuiltinMenuSurface::Background}) {
        auto rows = surface == BuiltinMenuSurface::Item ? BuildItemMenu(false, L"", false) : BuildBackgroundMenu(true, false, L"");
        rows.push_back(Row(CmdMapNetworkDrive)); prefs.SetBuiltinVisible(BuiltinMenuItem::MapNetworkDrive, false);
        ApplyBuiltinMenuPrefs(rows, prefs, surface);
        check(!Has(rows, CmdMapNetworkDrive) && WellFormed(rows), "shared network-drive toggle filters actual item and background menus");
    }
    prefs.builtin_hidden = 0;
    const Menu flyouts{NewFlyout(), GitFlyout()};
    for (const auto hidden : {BuiltinMenuItem::NewTemplates, BuiltinMenuItem::Git}) {
        prefs.builtin_hidden = BuiltinMenuBit(hidden);
        auto rows = flyouts; ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Background);
        const int hidden_command = hidden == BuiltinMenuItem::NewTemplates ? CmdShellNewBase : kCmdGitPull;
        const int retained_command = hidden == BuiltinMenuItem::NewTemplates ? kCmdGitPull : CmdShellNewBase;
        check(rows.size() == 1 && !Has(rows, hidden_command) && Has(rows, retained_command) && WellFormed(rows),
            "template and Git headers independently hide their entire submenu");
        prefs.builtin_hidden = 0; rows = flyouts; ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Background);
        check(Has(rows, hidden_command), "reenabling submenu restores its original commands");
    }
    auto rows = BuildBackgroundMenu(true, false, L""); rows.push_back(NewFlyout());
    prefs.builtin_hidden = BuiltinMenuBit(BuiltinMenuItem::NewFolder) | BuiltinMenuBit(BuiltinMenuItem::NewTextFile);
    ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Background);
    check(!Has(rows, CmdNewFolder) && !Has(rows, CmdNewTextFile) && Has(rows, CmdNewMarkdownFile) && Has(rows, CmdShellNewBase),
        "disabled folder and text creation do not survive inside the New flyout");
    auto empty = Row(CmdNone); empty.glyph = L"\xE710"; empty.children = {Row(CmdNewFolder), Row(CmdNewTextFile)};
    rows = {Row(CmdOpen), empty}; ApplyBuiltinMenuPrefs(rows, prefs);
    check(rows.size() == 1 && Has(rows, CmdOpen) && WellFormed(rows), "empty New flyout disappears without orphan separator");
    for (const auto hidden : {BuiltinMenuItem::CopyToWindow, BuiltinMenuItem::MoveToWindow}) {
        rows = BuildItemMenu(false, L"", false);
        AppendWindowTransferCommands(rows, {L"C:\\one", L"D:\\two"}, true);
        prefs.builtin_hidden = BuiltinMenuBit(hidden); ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
        const int gone = hidden == BuiltinMenuItem::CopyToWindow ? CmdCopyToWindowBase : CmdMoveToWindowBase;
        const int kept = hidden == BuiltinMenuItem::CopyToWindow ? CmdMoveToWindowBase : CmdCopyToWindowBase;
        check(!Has(rows, gone) && !Has(rows, gone + 1) && Has(rows, kept) && Has(rows, kept + 1) && WellFormed(rows),
            "real multi-window transfer flyouts obey independent settings");
        prefs.builtin_hidden = 0; rows = BuildItemMenu(false, L"", false);
        AppendWindowTransferCommands(rows, {L"C:\\one"}, false); ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
        check(Has(rows, gone), "reenabling window-transfer submenu preserves disabled move affordance");
    }
    prefs.builtin_hidden = (uint64_t{1} << kBuiltinMenuItemCount) - 1;
    rows = BuildItemMenu(false, L"", true); ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
    check(rows.size() == 2 && Has(rows, CmdOpen) && Has(rows, CmdCut) && Has(rows, CmdCopy) &&
        Has(rows, CmdDelete) && Has(rows, CmdRename) && rows[1].quick_swatches.size() == 4 && WellFormed(rows),
        "all optional rows can be hidden while Open and four fixed edit buttons remain");
    rows = {Row(CmdOpen), Row(CmdCut), Row(CmdCopy), Row(CmdDelete), Row(CmdRename)};
    ApplyBuiltinMenuPrefs(rows, prefs); check(rows.size() == 5, "fixed commands also survive as standalone rows");
    prefs.builtin_hidden = 0;
    prefs.SetBuiltinOrder(BuiltinMenuSurface::Item, {BuiltinMenuItem::StageToTray, BuiltinMenuItem::CopyPath, BuiltinMenuItem::Properties});
    rows = {Row(CmdOpen), Row(CmdCopyPath), Row(CmdStageToTray), Row(CmdProperties)};
    rows[1].separator_after = true;
    ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
    check(rows[0].command == CmdOpen && rows[1].command == CmdStageToTray && rows[2].command == CmdCopyPath &&
        rows[3].command == CmdProperties && rows[1].separator_after && !rows[2].separator_after && WellFormed(rows),
        "custom order moves newly configurable rows while keeping fixed row and separator slots");
    prefs.SetBuiltinVisible(BuiltinMenuItem::Properties, false); ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
    check(rows.size() == 3 && WellFormed(rows), "hiding final sorted row leaves no trailing separator");
    prefs.SetBuiltinVisible(BuiltinMenuItem::StageToTray, false); prefs.SetBuiltinVisible(BuiltinMenuItem::CopyPath, false);
    ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
    check(rows.size() == 1 && rows[0].command == CmdOpen && WellFormed(rows), "hiding a complete sorted group leaves no orphan separator");
    std::printf("Checks: %d Failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
