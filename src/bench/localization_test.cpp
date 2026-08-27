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
    passed &= Report("zh-CN resource selection",
                     Get(StringId::Settings) == L"\u8bbe\u7f6e" &&
                     Get(StringId::PreviewLoading) == L"\u6b63\u5728\u52a0\u8f7d\u9884\u89c8..." &&
                     Get(StringId::PreviewFit) == L"\u9002\u5e94" &&
                     Get(StringId::SettingsAboutDiagnostics) == L"\u5173\u4e8e\u4e0e\u8bca\u65ad" &&
                     Get(StringId::CheckForUpdates) == L"\u68c0\u67e5\u66f4\u65b0" &&
                     Get(StringId::RecycleBin) == L"\u56de\u6536\u7ad9" &&
                     Get(StringId::BatchRename) == L"\u6279\u91cf\u91cd\u547d\u540d" &&
                     Get(StringId::BatchRenameWillRename) == L"\u5c06\u6539\u540d" &&
                     Get(StringId::InvertSelection) == L"\u53cd\u9009");
    SetLanguage(L"en-US");
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
                     Get(StringId::ColumnDeleted) == L"Date deleted");
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
