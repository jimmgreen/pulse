#include "../common/localization.h"

#include <cstdio>
#include <atomic>
#include <thread>
#include <utility>
#include <string_view>

namespace {

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

bool CheckSystemLanguage() {
    using namespace pulse::l10n;
    struct Case { LANGID id; Language expected; const char* name; };
    const Case cases[] = {
        {0x0404, Language::ZhTW, "Taiwan maps to Traditional Chinese"},
        {0x0c04, Language::ZhTW, "Hong Kong maps to Traditional Chinese"},
        {0x1404, Language::ZhTW, "Macau maps to Traditional Chinese"},
        {0x7c04, Language::ZhTW, "Hant maps to Traditional Chinese"},
        {0x0804, Language::ZhCN, "PRC maps to Simplified Chinese"},
        {0x1004, Language::ZhCN, "Singapore maps to Simplified Chinese"},
        {0x0004, Language::ZhCN, "neutral Chinese maps to Simplified Chinese"},
        {0x0409, Language::EnUS, "US English maps to English"},
        {0x0809, Language::EnUS, "UK English maps to shipped English"},
        {0x0411, Language::EnUS, "unshipped locale falls back to English"},
    };
    bool passed = true;
    for (const auto& test : cases)
        passed &= Report(test.name, LanguageFromLangId(test.id) == test.expected);
    SetLanguage(L"system");
    passed &= Report("system preference follows actual Windows UI language",
        effective_language() == LanguageFromLangId(GetUserDefaultUILanguage()));
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    using namespace pulse::l10n;
    bool passed = true;
    Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc > 1) {
        if (argc == 2 && std::string_view(argv[1]) == "--system-language")
            return CheckSystemLanguage() ? 0 : 1;
        return 2;
    }
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
        bool search_complete = true;
        for (UINT id = IDS_SEARCH_ENH_0; id <= IDS_SEARCH_UI_1839; ++id)
            search_complete &= !Get(static_cast<StringId>(id)).empty();
        passed &= Report("search enhancement strings are present", search_complete);
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
    {
        // Ids above the last one Get() serves come back empty: keep kLastString current.
        bool newest = true;
        for (const auto language : {L"zh-CN", L"zh-TW", L"en-US"}) {
            SetLanguage(language);
            for (const auto id : {StringId::LanguageZhTW, StringId::SettingsAutoUpdate,
                                  StringId::SettingsAutoUpdateDesc, StringId::UpdateDescManual,
                                  StringId::SettingsUiFontSize, StringId::UiFontLarger,
                                  StringId::ListThumbnailBadges, StringId::ListThumbnailBadgesDesc,
                                  StringId::ContextOrderHintRow, StringId::SystemGroupNodeModules,
                                  StringId::HelpViewMode, StringId::HelpContextMenu})
                newest &= !Get(id).empty();
        }
        SetLanguage(L"zh-TW");
        newest &= Get(StringId::SettingsAutoUpdate) == L"\u81EA\u52D5\u6AA2\u67E5\u66F4\u65B0";  // 自動檢查更新
        SetLanguage(L"en-US");
        newest &= Get(StringId::SystemGroupNodeModules) == L"Development dependencies";
        newest &= Get(StringId::HelpContextMenu) == L"Open the context menu";
        passed &= Report("newest string ids load in every language", newest);
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
                     Get(StringId::DownloadUpdate) == L"Update and restart");
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
    // Text reported by helper processes (index service, pulse_shell) in Simplified.
    SetLanguage(L"en-US");
    passed &= Report("en-US service status with count and segment",
        ServiceText(L"已索引 1,234 项 · 实时更新") == L"1,234 items indexed · Live updates");
    passed &= Report("en-US service pattern keeps the path",
        ServiceText(L"正在重建 D:\\设计资料 的索引…") == L"Rebuilding the index for D:\\设计资料…");
    passed &= Report("en-US Win32 error keeps the system message",
        ServiceText(L"错误 5：拒绝访问。") == L"Error 5: 拒绝访问。");
    passed &= Report("en-US network state segments",
        ServiceText(L"离线 · 仍可搜索") == L"Offline · Still searchable");
    passed &= Report("en-US shell start failure",
        ServiceText(L"无法启动 pulse_shell.exe（错误 2）") == L"Could not start pulse_shell.exe (error 2)");
    passed &= Report("en-US unknown service text passes through",
        ServiceText(L"某个未知的状态") == L"某个未知的状态" && !IsKnownServiceText(L"某个未知的状态") &&
        IsKnownServiceText(L"已同步 · 128,420 项") && ServiceText(L"Ready") == L"Ready");
    SetLanguage(L"zh-TW");
    {
        // The path is a capture: it keeps its Simplified characters while the template converts.
        const std::wstring rebuilt = ServiceText(L"D:\\设计资料 的变更跟踪暂不可用，稍后重试");
        const std::wstring indexed = ServiceText(L"已索引 1,234 项 · 实时更新");
        passed &= Report("zh-TW service text converts the template, not the path",
            rebuilt.find(L"D:\\设计资料 ") == 0 && rebuilt.find(L"變更") != std::wstring::npos);
        passed &= Report("zh-TW service status keeps counts",
            indexed.find(L"1,234") != std::wstring::npos && indexed.find(L"項") != std::wstring::npos);
    }
    passed &= Report("zh-TW operation errors keep real file names",
        ServiceErrorText(L"拒绝访问。 | D:\\软件说明.txt") == L"拒绝访问。 | D:\\软件说明.txt" &&
        ServiceErrorText(L"目标文件夹不存在 | D:\\软件说明.txt").find(L" | D:\\软件说明.txt") != std::wstring::npos &&
        ServiceErrorText(L"目标文件夹不存在 | D:\\软件说明.txt") != L"目标文件夹不存在 | D:\\软件说明.txt");
    SetLanguage(L"en-US");
    passed &= Report("en-US operation error localizes the message only",
        ServiceErrorText(L"没有权限在此位置新建") != L"没有权限在此位置新建" &&
        ServiceErrorText(L"软件说明.txt 无法处理 | D:\\软件说明.txt") == L"软件说明.txt 无法处理 | D:\\软件说明.txt");
    SetLanguage(L"zh-CN");
    passed &= Report("zh-CN service text is unchanged",
        ServiceText(L"已索引 1,234 项 · 实时更新") == L"已索引 1,234 项 · 实时更新");

    passed &= CheckSystemLanguage();
    std::printf("\n== localization tests: %s ==\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
