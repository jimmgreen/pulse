#include "../common/windows_compat.h"
#include "app_prompts.h"

#include "app_state.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"

#include <cwchar>
#include <string>

namespace pulse {
namespace {

std::wstring Text(l10n::StringId id) { return l10n::Get(id); }

ui::ConfirmChoice Ask(AppState& s, const ui::ConfirmDialogSpec& spec) {
    return ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
}

// Older prompt strings carry their own paragraph breaks.
std::wstring TrimBreaks(std::wstring text) {
    while (!text.empty() && (text.front() == L'\n' || text.front() == L'\r')) text.erase(0, 1);
    return text;
}

} // namespace

bool ConfirmClearRecent(AppState& s) {
    ui::ConfirmDialogSpec spec;
    spec.title = Text(l10n::StringId::ClearRecentTitle);
    spec.message = Text(l10n::StringId::ClearRecentPrompt);
    spec.confirm_text = Text(l10n::StringId::ClearAll);
    spec.danger = true;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return Ask(s, spec) == ui::ConfirmChoice::Confirm;
}

bool ConfirmClearDiagnostics(AppState& s) {
    ui::ConfirmDialogSpec spec;
    spec.title = Text(l10n::StringId::ClearDiagnostics);
    spec.message = Text(l10n::StringId::DiagnosticsClearConfirm);
    spec.confirm_text = Text(l10n::StringId::ClearDiagnostics);
    spec.danger = true;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return Ask(s, spec) == ui::ConfirmChoice::Confirm;
}

bool ConfirmDiagnosticsExport(AppState& s, bool& include_service, bool& include_dumps) {
    ui::ConfirmDialogSpec privacy;
    privacy.title = Text(l10n::StringId::DiagnosticsPrivacyTitle);
    privacy.message = l10n::Pick(L"导出脱敏错误事件、程序版本、服务状态和磁盘空间，供反馈问题。默认不包含文件路径、搜索内容或内存转储。若需要排查崩溃，可选择包含转储；转储可能包含私人信息。请检查生成的 ZIP 后再发送。", L"Export sanitized error events, application versions, service status and disk space. File paths, searches and memory dumps are excluded by default. For crash analysis, you can include dumps, which may contain private information. Review the ZIP before sharing.");
    privacy.confirm_text = l10n::Pick(L"导出普通诊断", L"Export basic diagnostics");
    privacy.secondary_text = l10n::Pick(L"同时包含崩溃转储", L"Include crash dumps");
    privacy.tone = ui::ConfirmTone::Warning;
    privacy.default_choice = ui::ConfirmChoice::Cancel;
    const auto privacy_choice = Ask(s, privacy);
    if (privacy_choice == ui::ConfirmChoice::Cancel) return false;
    include_dumps = privacy_choice == ui::ConfirmChoice::Secondary;

    ui::ConfirmDialogSpec service;
    service.title = Text(l10n::StringId::DiagnosticsPrivacyTitle);
    service.message = Text(l10n::StringId::DiagnosticsIncludeService);
    service.confirm_text = Text(l10n::StringId::DiagnosticsIncludeServiceYes);
    service.secondary_text = Text(l10n::StringId::DiagnosticsIncludeServiceNo);
    service.tone = ui::ConfirmTone::Question;
    service.glyph = L"\xE7EF";  // Admin (shield)
    service.default_choice = ui::ConfirmChoice::Secondary;
    const ui::ConfirmChoice choice = Ask(s, service);
    if (choice == ui::ConfirmChoice::Cancel) return false;
    include_service = choice == ui::ConfirmChoice::Confirm;
    return true;
}

bool AskRetryRecovery(AppState& s, size_t count, bool uncertain_destructive, bool duplicate_cleanup) {
    ui::ConfirmDialogSpec spec;
    spec.title = Text(l10n::StringId::RecoveryTitle);
    wchar_t message[512]{};
    swprintf_s(message, Text(l10n::StringId::RecoveryPromptFormat).c_str(), count);
    spec.message = message;
    if (uncertain_destructive) spec.note = TrimBreaks(Text(l10n::StringId::RecoveryDestructiveWarning));
    if (duplicate_cleanup) {
        if (!spec.note.empty()) spec.note += L"\n";
        spec.note += l10n::Pick(L"重复文件清理不会恢复，请重新扫描。",
                                L"Duplicate cleanup will not be resumed. Please scan again.");
    }
    spec.confirm_text = Text(l10n::StringId::RecoveryRetry);
    spec.cancel_text = Text(l10n::StringId::RecoveryDiscard);
    spec.tone = ui::ConfirmTone::Warning;
    spec.glyph = L"\xE777";  // UpdateRestore
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return Ask(s, spec) == ui::ConfirmChoice::Confirm;
}

} // namespace pulse
