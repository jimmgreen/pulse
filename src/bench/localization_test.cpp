#include "../common/localization.h"

#include <cstdio>
#include <atomic>
#include <thread>

namespace {

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

} // namespace

int main() {
    using namespace pulse::l10n;
    bool passed = true;
    Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto& held_chinese = Get(StringId::TabRename);
    SetLanguage(L"en-US");
    passed &= Report("language switch preserves strings held by worker tasks",
        held_chinese == L"重命名标签页" && Get(StringId::TabRename) == L"Rename tab");
    std::atomic<bool> worker_ok{true};
    std::thread reader([&] {
        for (int i = 0; i < 1000; ++i) {
            const auto& label = Get(StringId::TabRename);
            if (label != L"重命名标签页" && label != L"Rename tab") worker_ok = false;
        }
    });
    for (int i = 0; i < 100; ++i) SetLanguage(i % 2 ? L"en-US" : L"zh-CN");
    reader.join();
    passed &= Report("worker localization remains valid during language switches", worker_ok.load());
    for (const auto language : {L"zh-CN", L"en-US"}) {
        SetLanguage(language);
        bool complete = true;
        for (UINT id = 1500; id <= 1719; ++id) {
            if ((id > 1524 && id < 1550) || (id > 1573 && id < 1600) ||
                (id > 1685 && id < 1700)) continue;
            complete &= !Get(static_cast<StringId>(id)).empty();
        }
        passed &= Report("audited menus dialogs and status strings are present", complete);
        const bool chinese = std::wstring(language) == L"zh-CN";
        passed &= Report("tag and badge menus follow display language",
            Get(StringId::TagSearchHint) == (chinese ? L"搜索或新建标签…" : L"Search or create a tag…") &&
            Get(StringId::ColorRed) == (chinese ? L"红色" : L"Red") &&
            Get(StringId::EditBadge) == (chinese ? L"编辑徽章" : L"Edit badge") &&
            Get(StringId::Unstar) == (chinese ? L"取消星标" : L"Unstar"));
        wchar_t tag_message[256]{};
        const auto& tag_pattern = Get(StringId::TagDeleteUsedFormat);
        swprintf_s(tag_message, tag_pattern.c_str(), L"100% 工作", size_t{3});
        passed &= Report("tag deletion preserves user names and association count",
            std::wstring(tag_message) == (chinese
                ? L"“100% 工作”已用于 3 个项目。\n删除后将移除这些关联。"
                : L"“100% 工作” is used by 3 items.\nDeleting it will remove these associations."));
        passed &= Report("tab menu TabNewRight follows display language",
            Get(StringId::TabNewRight) == (std::wstring(language) == L"zh-CN" ? L"在右侧新建标签页" : L"New tab to the right"));
        passed &= Report("tab menu TabDuplicate follows display language",
            Get(StringId::TabDuplicate) == (std::wstring(language) == L"zh-CN" ? L"复制标签页" : L"Duplicate tab"));
        passed &= Report("tab menu TabPin follows display language",
            Get(StringId::TabPin) == (std::wstring(language) == L"zh-CN" ? L"固定标签页" : L"Pin tab"));
        passed &= Report("tab menu TabUnpin follows display language",
            Get(StringId::TabUnpin) == (std::wstring(language) == L"zh-CN" ? L"取消固定标签页" : L"Unpin tab"));
        passed &= Report("tab menu TabCreateGroup follows display language",
            Get(StringId::TabCreateGroup) == (std::wstring(language) == L"zh-CN" ? L"创建新组" : L"Create group"));
        passed &= Report("tab menu TabNewGroup follows display language",
            Get(StringId::TabNewGroup) == (std::wstring(language) == L"zh-CN" ? L"将标签页添加到新组" : L"Add tab to new group"));
        passed &= Report("tab menu TabJoinGroup follows display language",
            Get(StringId::TabJoinGroup) == (std::wstring(language) == L"zh-CN" ? L"将标签页添加到" : L"Add tab to group"));
        passed &= Report("tab menu TabUnnamedGroup follows display language",
            Get(StringId::TabUnnamedGroup) == (std::wstring(language) == L"zh-CN" ? L"(未命名组)" : L"(Unnamed group)"));
        passed &= Report("tab menu TabRemoveGroup follows display language",
            Get(StringId::TabRemoveGroup) == (std::wstring(language) == L"zh-CN" ? L"从组中移除该标签页" : L"Remove tab from group"));
        passed &= Report("tab menu TabClose follows display language",
            Get(StringId::TabClose) == (std::wstring(language) == L"zh-CN" ? L"关闭标签页" : L"Close tab"));
        passed &= Report("tab menu TabCloseOthers follows display language",
            Get(StringId::TabCloseOthers) == (std::wstring(language) == L"zh-CN" ? L"关闭其他标签页" : L"Close other tabs"));
        passed &= Report("tab menu TabCloseRight follows display language",
            Get(StringId::TabCloseRight) == (std::wstring(language) == L"zh-CN" ? L"关闭右侧标签页" : L"Close tabs to the right"));
        passed &= Report("tab menu TabGroupName follows display language",
            Get(StringId::TabGroupName) == (std::wstring(language) == L"zh-CN" ? L"标签组名称…" : L"Tab group name…"));
        passed &= Report("tab menu TabGroupNew follows display language",
            Get(StringId::TabGroupNew) == (std::wstring(language) == L"zh-CN" ? L"在组中新建标签页" : L"New tab in group"));
        passed &= Report("tab menu TabUngroup follows display language",
            Get(StringId::TabUngroup) == (std::wstring(language) == L"zh-CN" ? L"取消组合" : L"Ungroup"));
        passed &= Report("tab menu TabGroupClose follows display language",
            Get(StringId::TabGroupClose) == (std::wstring(language) == L"zh-CN" ? L"关闭分组标签页" : L"Close group"));
        passed &= Report("tab menu TabMenuSearch follows display language",
            Get(StringId::TabMenuSearch) == (std::wstring(language) == L"zh-CN" ? L"搜索命令、文件夹…" : L"Search commands, folders…"));
        passed &= Report("tab identity menu and settings labels are translated",
            !Get(StringId::PinnedNames).empty() && !Get(StringId::PinnedNamesDesc).empty() &&
            !Get(StringId::TabRename).empty() && !Get(StringId::TabNameHint).empty() &&
            !Get(StringId::TabNameSave).empty() && !Get(StringId::TabNameReset).empty() &&
            !Get(StringId::TabColor).empty() && !Get(StringId::TabColorNone).empty());
        passed &= Report("update installation states are translated",
            !Get(StringId::DownloadingUpdate).empty() && !Get(StringId::InstallingUpdate).empty() &&
            !Get(StringId::UpdateClickToInstall).empty() && !Get(StringId::UpdateCancelled).empty() &&
            !Get(StringId::UpdateBusy).empty() && !Get(StringId::UpdateInstallFailed).empty());
    }
    SetLanguage(L"zh-CN");
    passed &= Report("zh-CN quick access and compatibility resources",
        Get(StringId::PinQuickAccess) == L"固定到快速访问" &&
        Get(StringId::UnpinQuickAccess) == L"从快速访问取消固定" &&
        !Get(StringId::EffectUnavailable).empty());
    passed &= Report("zh-CN sorting commands", Get(StringId::SortBy) == L"排序方式" &&
        Get(StringId::SortAscending) == L"升序" && Get(StringId::SortDescending) == L"降序");
    passed &= Report("zh-CN resource selection",
                     Get(StringId::Settings) == L"\u8bbe\u7f6e" &&
                     Get(StringId::PreviewLoading) == L"\u6b63\u5728\u52a0\u8f7d\u9884\u89c8..." &&
                     Get(StringId::PreviewFit) == L"\u9002\u5e94" &&
                     Get(StringId::SettingsAboutDiagnostics) == L"\u5173\u4e8e\u4e0e\u8bca\u65ad" &&
                     Get(StringId::CheckForUpdates) == L"\u68c0\u67e5\u66f4\u65b0" &&
                     Get(StringId::RecycleBin) == L"\u56de\u6536\u7ad9" &&
                     Get(StringId::BatchRename) == L"\u6279\u91cf\u91cd\u547d\u540d" &&
                     Get(StringId::BatchRenameWillRename) == L"\u5c06\u6539\u540d" &&
                     Get(StringId::InvertSelection) == L"\u53cd\u9009" &&
                     Get(StringId::SettingsDuplicates) == L"\u91cd\u590d\u6587\u4ef6" &&
                     Get(StringId::DupScan) == L"\u626b\u63cf" &&
                     Get(StringId::DupMinSize) == L"\u6700\u5c0f\u5927\u5c0f" &&
                     Get(StringId::PinWorkspace) == L"\u9489\u4e3a\u5de5\u4f5c\u533a" &&
                     Get(StringId::UnpinWorkspace) == L"\u53d6\u6d88\u5de5\u4f5c\u533a" &&
                     Get(StringId::AdvancedSearch) == L"\u9ad8\u7ea7\u641c\u7d22" &&
                     Get(StringId::KindCustom) == L"\u6307\u5b9a\u6269\u5c55\u540d" &&
                     Get(StringId::OpEmptying) == L"\u6b63\u5728\u6e05\u7a7a\u56de\u6536\u7ad9" &&
                     Get(StringId::StatusHintIdle) ==
                         L"\u7a7a\u683c \u9884\u89c8  \u00b7  Ctrl+F \u7b5b\u9009  \u00b7  Ctrl+K \u641c\u7d22" &&
                     Get(StringId::SettingsShowPerformance) ==
                         L"\u5728\u72b6\u6001\u680f\u663e\u793a\u6027\u80fd");
    SetLanguage(L"en-US");
    passed &= Report("en-US quick access and compatibility resources",
        Get(StringId::PinQuickAccess) == L"Pin to Quick access" &&
        Get(StringId::UnpinQuickAccess) == L"Unpin from Quick access" &&
        !Get(StringId::EffectUnavailable).empty());
    passed &= Report("en-US sorting commands", Get(StringId::SortBy) == L"Sort by" &&
        Get(StringId::SortAscending) == L"Ascending" && Get(StringId::SortDescending) == L"Descending");
    passed &= Report("en-US resource selection",
                     Get(StringId::Settings) == L"Settings" &&
                     Get(StringId::PreviewFit) == L"Fit" &&
                     Get(StringId::PreviewLoading) == L"Loading preview...");
    passed &= Report("en-US main shell resources",
                     Get(StringId::SidebarQuickAccess) == L"Quick access" &&
                     Get(StringId::ColumnModified) == L"Date modified" &&
                     Get(StringId::NotSelected) == L"Nothing selected");
    passed &= Report("en-US command resources",
                     Get(StringId::TooltipCloseTab) == L"Close tab" &&
                     Get(StringId::OpenTerminalHere) == L"Open terminal here" &&
                     Get(StringId::SettingsAboutDiagnostics) == L"About & diagnostics" &&
                     Get(StringId::DownloadUpdate) == L"Download and install");
    passed &= Report("en-US recycle and batch rename",
                     Get(StringId::RecycleBin) == L"Recycle Bin" &&
                     Get(StringId::BatchRename) == L"Batch rename" &&
                     Get(StringId::BatchRenameWillRename) == L"Will rename" &&
                     Get(StringId::InvertSelection) == L"Invert selection" &&
                     Get(StringId::ColumnDeleted) == L"Date deleted" &&
                     Get(StringId::SettingsDuplicates) == L"Duplicate files" &&
                     Get(StringId::DupDeleteAllExtras) == L"Delete all extras" &&
                     Get(StringId::PinWorkspace) == L"Pin as workspace" &&
                     Get(StringId::UnpinWorkspace) == L"Unpin workspace" &&
                     Get(StringId::AdvancedSearch) == L"Advanced search" &&
                     Get(StringId::KindCustom) == L"Custom extension" &&
                     Get(StringId::OpEmptying) == L"Emptying Recycle Bin" &&
                     Get(StringId::OpPause) == L"Pause" &&
                     Get(StringId::StatusHintIdle) ==
                         L"Space preview  \u00b7  Ctrl+F filter  \u00b7  Ctrl+K search" &&
                     Get(StringId::SettingsShowPerformance) ==
                         L"Show performance in the status bar");
    passed &= Report("stable language identifiers",
                     IsLanguageId(L"system") && IsLanguageId(L"zh-CN") &&
                     IsLanguageId(L"en-US") && !IsLanguageId(L"english") &&
                     std::wstring(LanguageId(Language::EnUS)) == L"en-US");
    SetLanguage(L"system");
    passed &= Report("system language resolves to a shipped locale",
                     effective_language() == Language::ZhCN ||
                     effective_language() == Language::EnUS);
    std::printf("\n== localization tests: %s ==\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
