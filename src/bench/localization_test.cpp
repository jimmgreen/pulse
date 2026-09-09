#include "../common/localization.h"

#include <cstdio>

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
                     Get(StringId::DownloadUpdate) == L"Open download page");
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
