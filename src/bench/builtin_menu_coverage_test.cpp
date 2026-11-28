#include "../app/context_menu.h"
#include "../app/git_ui.h"
#include "../app/app_model.h"
#include "../common/localization.h"
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
    row.children = {Row(kCmdGitSourceControl), Row(kCmdGitPull), Row(kCmdGitPush), Row(kCmdGitRefresh)}; return row;
}
ui::FluentMenuItem GitFlyoutBranchGlyph() {
    auto row = Row(CmdNone); row.glyph = L"\xF003";
    row.children = {Row(kCmdGitSourceControl), Row(kCmdGitHistory), Row(kCmdGitBranches),
                    Row(kCmdGitSync), Row(kCmdGitPull), Row(kCmdGitPush), Row(kCmdGitRefresh)};
    return row;
}
}
int main(int argc, char** argv) {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0, checks = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks; failures += !value; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    };
    if (argc > 1 && std::string_view(argv[1]) == "--native-explorer") {
        constexpr auto native = BuiltinMenuItem::OpenNativeExplorer;
        check(static_cast<unsigned>(native) == static_cast<unsigned>(BuiltinMenuItem::MoveToWindow) + 1 &&
              BuiltinMenuKey(native) == L"open_native_explorer", "native builtin appends a stable key without renumbering old mask bits");
        check(BuiltinItemForCommand(CmdOpenNativeExplorer) == native &&
              BuiltinItemForCommand(CmdOpenNativeExplorerHere) == native,
              "item and background commands share one settings visibility owner");
        check(!Has(BuildItemMenu(false, L""), CmdOpenNativeExplorer) &&
              !Has(BuildBackgroundMenu(false, false, L""), CmdOpenNativeExplorerHere),
              "builders omit native entry unless the actual context opts in");
        const auto file_menu = BuildItemMenu(false, L"", false, true);
        const auto folder_menu = BuildItemMenu(false, L"", true, true);
        const auto background_menu = BuildBackgroundMenu(false, false, L"", true);
        check(Has(file_menu, CmdOpenNativeExplorer) && Has(folder_menu, CmdOpenNativeExplorer) &&
              Has(background_menu, CmdOpenNativeExplorerHere) && WellFormed(file_menu) && WellFormed(background_menu),
              "real file folder and background builders expose the native command");
        ContextMenuPrefs prefs; prefs.persist = false;
        prefs.SetBuiltinVisible(native, false);
        const auto hidden_json = prefs.ToJson();
        ContextMenuPrefs loaded; loaded.persist = false;
        check(loaded.FromJson(hidden_json) && !loaded.BuiltinVisible(native) &&
              hidden_json.find(L"open_native_explorer") != std::wstring::npos,
              "hidden native command survives the real JSON round trip");
        auto item = file_menu, background = background_menu;
        ApplyBuiltinMenuPrefs(item, loaded, BuiltinMenuSurface::Item);
        ApplyBuiltinMenuPrefs(background, loaded, BuiltinMenuSurface::Background);
        check(!Has(item, CmdOpenNativeExplorer) && !Has(background, CmdOpenNativeExplorerHere) && Has(item, CmdOpen),
              "one settings switch hides both native commands while preserving fixed Open");
        loaded.SetBuiltinVisible(native, true);
        // Settings drag supplies a complete order. Partial lists are migration
        // input: normalization inserts missing defaults around their predecessors.
        auto item_order = BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item);
        auto background_order = BuiltinMenuDefaultOrder(BuiltinMenuSurface::Background);
        for (auto* order : {&item_order, &background_order}) {
            std::erase(*order, native);
            std::erase(*order, BuiltinMenuItem::CopyPath);
        }
        item_order.insert(item_order.begin(), {BuiltinMenuItem::CopyPath, native});
        background_order.insert(background_order.begin(), {native, BuiltinMenuItem::CopyPath});
        loaded.SetBuiltinOrder(BuiltinMenuSurface::Item, item_order);
        loaded.SetBuiltinOrder(BuiltinMenuSurface::Background, background_order);
        ContextMenuPrefs ordered; ordered.persist = false;
        check(ordered.FromJson(loaded.ToJson()) && ordered.BuiltinVisible(native) &&
              ordered.BuiltinOrder(BuiltinMenuSurface::Item)[0] == BuiltinMenuItem::CopyPath &&
              ordered.BuiltinOrder(BuiltinMenuSurface::Background)[0] == native,
              "restore and independent per-surface order survive JSON");
        item = file_menu; background = background_menu;
        ApplyBuiltinMenuPrefs(item, ordered, BuiltinMenuSurface::Item);
        ApplyBuiltinMenuPrefs(background, ordered, BuiltinMenuSurface::Background);
        const auto position = [](const Menu& menu, int command) {
            return std::find_if(menu.begin(), menu.end(), [=](const auto& row) { return row.command == command; }) - menu.begin();
        };
        check(position(item, CmdCopyPath) < position(item, CmdOpenNativeExplorer) && item.front().command == CmdOpen &&
              position(background, CmdOpenNativeExplorerHere) < position(background, CmdCopyPath),
              "production ordering moves the native row independently on both surfaces");
        const auto file = NativeExplorerMenuRequest(CmdOpenNativeExplorer, L"pulse:search:", {{L"C:\\fixture\\report.txt", false}});
        check(file && file->intent == native_explorer::Intent::SelectFile && file->path == L"C:\\fixture\\report.txt",
              "selected real file dispatch requests native Explorer selection even from search");
        const auto folder = NativeExplorerMenuRequest(CmdOpenNativeExplorer, L"C:\\fixture", {{L"C:\\fixture\\folder", true}});
        check(folder && folder->intent == native_explorer::Intent::OpenFolder && folder->path == L"C:\\fixture\\folder",
              "selected folder dispatch opens that folder");
        const auto here = NativeExplorerMenuRequest(CmdOpenNativeExplorerHere, L"C:\\current", {{L"C:\\other\\kept.txt", false}, {L"C:\\other\\also.txt", false}});
        check(here && here->intent == native_explorer::Intent::OpenFolder && here->path == L"C:\\current",
              "background dispatch ignores even a retained multi-selection and opens the current directory");
        check(!NativeExplorerMenuRequest(CmdOpenNativeExplorer, L"C:\\current", {}) &&
              !NativeExplorerMenuRequest(CmdOpenNativeExplorer, L"C:\\current", {{L"C:\\a", false}, {L"C:\\b", true}}),
              "item dispatch rejects empty and multiple selections instead of silently picking one");
        bool rejects_virtual = true;
        for (const auto* path : {L"pulse:vfs:archive", L"device:phone", L"shell:Downloads", L"relative.txt", L""}) {
            rejects_virtual &= !NativeExplorerMenuRequest(CmdOpenNativeExplorerHere, path, {});
            rejects_virtual &= !NativeExplorerMenuRequest(CmdOpenNativeExplorer, L"C:\\current", {{path, false}});
        }
        check(rejects_virtual, "virtual device shell and relative paths cannot create a native request");
        check(!NativeExplorerMenuRequest(CmdOpen, L"C:\\current", {{L"C:\\a", false}}),
              "unrelated commands cannot dispatch the native Explorer action");
        std::printf("Checks: %d Failures: %d\n", checks, failures);
        return failures ? 1 : 0;
    }
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
        {CmdConnectServer, BuiltinMenuItem::ConnectServer}, {CmdUnpinNetwork, BuiltinMenuItem::PinNetwork},
        {CmdOpenInOtherPane, BuiltinMenuItem::OpenInOtherPane}, {CmdOpenInOtherPaneHere, BuiltinMenuItem::OpenInOtherPane},
        {CmdCopyToPicked, BuiltinMenuItem::CopyToWindow}, {CmdMoveToPicked, BuiltinMenuItem::MoveToWindow}
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
    {
        const Menu modern_flyouts{NewFlyout(), GitFlyoutBranchGlyph()};
        ContextMenuPrefs p2; p2.persist = false;
        p2.builtin_hidden = BuiltinMenuBit(BuiltinMenuItem::Git);
        auto r2 = modern_flyouts; ApplyBuiltinMenuPrefs(r2, p2, BuiltinMenuSurface::Background);
        check(r2.size() == 1 && !Has(r2, kCmdGitSync) && !Has(r2, kCmdGitBranches) &&
              Has(r2, CmdShellNewBase) && WellFormed(r2),
              "modern F003 Git flyout maps to BuiltinMenuItem::Git and hides independently");
        p2.builtin_hidden = 0; r2 = modern_flyouts; ApplyBuiltinMenuPrefs(r2, p2, BuiltinMenuSurface::Background);
        check(Has(r2, kCmdGitSync) && Has(r2, kCmdGitHistory),
              "reenabling modern F003 Git flyout restores Sync and History items");
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
        rows = BuildItemMenu(false, L"", false);
        AppendWindowTransferCommands(rows, {L"C:\\pane", L"D:\\window"}, true, 1, true);
        prefs.builtin_hidden = BuiltinMenuBit(hidden); ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
        const int gone_pick = hidden == BuiltinMenuItem::CopyToWindow ? CmdCopyToPicked : CmdMoveToPicked;
        const int kept_pick = hidden == BuiltinMenuItem::CopyToWindow ? CmdMoveToPicked : CmdCopyToPicked;
        check(!Has(rows, gone_pick) && Has(rows, kept_pick) && Has(rows, kept + 1) && WellFormed(rows),
            "选择文件夹… and the pane rows go with their 复制到 / 移动到 flyout");
        prefs.builtin_hidden = 0;
    }
    {
        const auto order = BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item);
        const auto background_order = BuiltinMenuDefaultOrder(BuiltinMenuSurface::Background);
        const auto has = [](const auto& list, BuiltinMenuItem item) { return std::find(list.begin(), list.end(), item) != list.end(); };
        check(has(order, BuiltinMenuItem::OpenInOtherPane) && has(background_order, BuiltinMenuItem::OpenInOtherPane) &&
              !has(order, BuiltinMenuItem::CopyToTarget) && !has(order, BuiltinMenuItem::MoveToTarget),
              "在另一栏打开 is listed on both surfaces; 复制到 / 移动到目标栏 left the item menu");
        prefs.builtin_hidden = 0;
        Menu other{Row(CmdOpen), Row(CmdOpenInOtherPane)};
        prefs.SetBuiltinVisible(BuiltinMenuItem::OpenInOtherPane, false);
        ApplyBuiltinMenuPrefs(other, prefs, BuiltinMenuSurface::Item);
        check(!Has(other, CmdOpenInOtherPane) && Has(other, CmdOpen), "在另一栏打开 follows its settings switch");
        prefs.SetBuiltinVisible(BuiltinMenuItem::OpenInOtherPane, true);
    }
    prefs.builtin_hidden = (uint64_t{1} << kBuiltinMenuItemCount) - 1;
    rows = BuildItemMenu(false, L"", true); ApplyBuiltinMenuPrefs(rows, prefs, BuiltinMenuSurface::Item);
    check(rows.size() == 2 && Has(rows, CmdOpen) && Has(rows, CmdCut) && Has(rows, CmdCopy) &&
        Has(rows, CmdDelete) && Has(rows, CmdRename) && rows[1].quick_swatches.size() == 4 && WellFormed(rows),
        "all optional rows can be hidden while Open and four fixed edit buttons remain");
    {
        const auto compact = BuildItemMenu(false, L"", true);
        const auto large = BuildItemMenu(false, L"", true, false, true);
        const auto& plain_strip = compact[1].quick_swatches;
        const auto& big_strip = large[1].quick_swatches;
        check(!plain_strip.empty() && plain_strip[0].label.empty() && !plain_strip[0].large_button &&
              big_strip.size() == 4 && big_strip[0].large_button &&
              !big_strip[0].label.empty() && !big_strip[1].label.empty() &&
              !big_strip[2].label.empty() && !big_strip[3].label.empty() &&
              big_strip[0].command == CmdCut && big_strip[3].command == CmdRename && WellFormed(large),
              "large-icon switch names all four action buttons without changing their commands");
    }
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
    {
        SidebarDriveMenuOptions fixed;
        auto menu = BuildSidebarDriveMenu(fixed);
        check(Has(menu, CmdOpen) && Has(menu, CmdOpenInNewTab) && Has(menu, CmdCopyPath) &&
              Has(menu, CmdOpenTerminal) && Has(menu, CmdProperties) && Has(menu, CmdPinWorkspace) &&
              !Has(menu, CmdCopy) && !Has(menu, CmdEjectDrive) && !Has(menu, CmdDisconnectDrive) &&
              !Has(menu, CmdUnlockDrive) && WellFormed(menu),
              "sidebar drive menu for a fixed drive omits Copy and drive-specific verbs");
        SidebarDriveMenuOptions ejectable; ejectable.ejectable = true; ejectable.workspace_pinned = true;
        menu = BuildSidebarDriveMenu(ejectable);
        check(Has(menu, CmdEjectDrive) && !Has(menu, CmdDisconnectDrive) && !Has(menu, CmdUnlockDrive) &&
              menu.size() >= 2 && menu[0].command == CmdOpen && menu[1].command == CmdEjectDrive &&
              WellFormed(menu),
              "sidebar drive menu for an ejectable drive places Eject right after Open");
        SidebarDriveMenuOptions remote; remote.remote = true;
        menu = BuildSidebarDriveMenu(remote);
        check(Has(menu, CmdDisconnectDrive) && !Has(menu, CmdEjectDrive) && WellFormed(menu),
              "sidebar drive menu for a mapped network drive offers Disconnect");
        SidebarDriveMenuOptions locked; locked.locked = true; locked.ejectable = true;
        menu = BuildSidebarDriveMenu(locked);
        check(Has(menu, CmdOpen) && Has(menu, CmdUnlockDrive) && Has(menu, CmdEjectDrive) &&
              Has(menu, CmdCopyPath) && Has(menu, CmdProperties) && !Has(menu, CmdOpenInNewTab) &&
              !Has(menu, CmdOpenTerminal) && !Has(menu, CmdPinWorkspace) && WellFormed(menu),
              "sidebar drive menu for a locked drive offers Unlock/Eject and omits open-in-tab/terminal/workspace");
        ContextMenuPrefs drive_prefs; drive_prefs.persist = false;
        drive_prefs.SetBuiltinVisible(BuiltinMenuItem::EjectDrive, false);
        menu = BuildSidebarDriveMenu(ejectable);
        ApplyBuiltinMenuPrefs(menu, drive_prefs, BuiltinMenuSurface::Item);
        check(!Has(menu, CmdEjectDrive) && Has(menu, CmdOpen) && WellFormed(menu),
              "sidebar drive menu respects builtin context-menu visibility prefs");
    }
    std::printf("Checks: %d Failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
