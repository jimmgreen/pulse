#include "../common/app_runtime_paths.h"
#include "../common/windows_compat.h"
#include "settings_transfer_ui.h"

#include "app_commands.h"
#include "app_messages.h"
#include "app_state.h"
#include "pulse_version.h"
#include "session.h"
#include "settings_controller.h"
#include "settings_transfer.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../ui/confirm_dialog.h"

#include <cwchar>
#include <mutex>
#include <utility>
#include <vector>

namespace pulse {
namespace {

namespace st = app::settings_transfer;
using l10n::StringId;

// The card's view of the data dir: read once, then again after each action
// (only this process stages imports or makes backups).
struct DiskState {
    bool pending = false;
    bool has_backup = false;
    std::wstring backup_dir;
    std::wstring backup_time;
};
std::mutex g_disk_mutex;
DiskState g_disk;
bool g_disk_loaded = false;
st::ApplyResult g_startup;  // what this start applied, for its toast

constexpr StringId kCategoryText[st::kCategoryCount] = {
    StringId::TransferCatPrefs, StringId::TransferCatPlaces, StringId::TransferCatTags,
    StringId::TransferCatAliases, StringId::TransferCatSearches,
};

std::wstring Replace(std::wstring text, std::wstring_view key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

// "20261021-234005" (backup names) or "2026-10-21T23:40:05" (bundles).
std::wstring LocalStamp(bool iso) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[32]{};
    swprintf_s(text, iso ? L"%04u-%02u-%02uT%02u:%02u:%02u" : L"%04u%02u%02u-%02u%02u%02u",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return text;
}

void RefreshDisk() {
    const std::wstring dir = app::GetPulseDataDir();
    DiskState next;
    next.pending = st::HasPendingImport(dir);
    const auto backups = st::ListBackups(dir);
    if (!backups.empty()) {
        next.has_backup = true;
        next.backup_dir = backups.front().dir;
        next.backup_time = st::DisplayTime(backups.front().stamp);
    }
    std::lock_guard lock(g_disk_mutex);
    g_disk = std::move(next);
    g_disk_loaded = true;
}

DiskState Disk() {
    {
        std::lock_guard lock(g_disk_mutex);
        if (g_disk_loaded) return g_disk;
    }
    RefreshDisk();
    std::lock_guard lock(g_disk_mutex);
    return g_disk;
}

void Toast(AppState& s, const std::wstring& message) {
    s.notification_toast.Show(s.hwnd, l10n::Get(StringId::SettingsTransfer), message);
}

ui::ConfirmChoice Ask(AppState& s, const ui::ConfirmDialogSpec& spec) {
    return ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
}

void ShowProblem(AppState& s, StringId title, StringId message) {
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(title);
    spec.message = l10n::Get(message);
    spec.single_button = true;
    spec.tone = ui::ConfirmTone::Warning;
    Ask(s, spec);
}

struct Choice {
    ui::ConfirmChoice button = ui::ConfirmChoice::Cancel;
    uint32_t categories = 0;
};

// One check box row per category in `offered`, all ticked to begin with.
Choice AskCategories(AppState& s, ui::ConfirmDialogSpec spec, uint32_t offered) {
    std::vector<st::Category> shown;
    for (int i = 0; i < st::kCategoryCount; ++i) {
        const st::Category category = st::CategoryAt(i);
        if (!(offered & category)) continue;
        shown.push_back(category);
        spec.items.push_back(l10n::Get(kCategoryText[i]));
    }
    std::vector<bool> checks(shown.size(), true);
    spec.item_checks = &checks;
    Choice choice;
    if (shown.empty()) return choice;
    choice.button = Ask(s, spec);
    if (choice.button == ui::ConfirmChoice::Cancel) return choice;
    for (size_t i = 0; i < shown.size(); ++i)
        if (checks[i]) choice.categories |= shown[i];
    return choice;
}

void Start(AppState& s, app::SettingsTask task, StringId failure) {
    if (!s.settings.StartSettingsTransfer(std::move(task))) Toast(s, l10n::Get(failure));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ExportSettings(AppState& s) {
    const uint32_t available = st::AvailableCategories(app::GetPulseDataDir());
    if (!available) {
        Toast(s, l10n::Get(StringId::SettingsExportFailed));
        return;
    }
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(StringId::ExportSettingsTitle);
    spec.message = l10n::Get(StringId::ExportSettingsPrompt);
    spec.confirm_text = l10n::Get(StringId::ExportSettingsConfirm);
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xEDE1";  // Export
    const Choice choice = AskCategories(s, spec, available);
    if (choice.button != ui::ConfirmChoice::Confirm || !choice.categories) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[160]{};
    swprintf_s(name, L"%s %04u-%02u-%02u.json", l10n::Get(StringId::ExportSettingsFileName).c_str(),
               now.wYear, now.wMonth, now.wDay);
    std::wstring file;
    if (!PickJsonFile(s.hwnd, true, name, file)) return;
    app::SettingsTask task{app::SettingsTaskKind::SettingsExport};
    task.path = std::move(file);
    task.key = std::to_wstring(choice.categories);
    Start(s, std::move(task), StringId::SettingsExportFailed);
}

void ImportSettings(AppState& s) {
    std::wstring file;
    if (!PickJsonFile(s.hwnd, false, nullptr, file)) return;
    // A settings file is a few hundred KB at most; it is read here, like
    // 导入别名, so the questions follow without a round trip. The task reads
    // it again when it stages the import.
    std::wstring text;
    st::Bundle bundle;
    const st::ParseResult parsed = ReadUtf8File(file, text) ? st::ParseBundle(text, bundle)
                                                            : st::ParseResult::Invalid;
    if (parsed != st::ParseResult::Ok) {
        ShowProblem(s, StringId::ImportSettingsTitle,
                    parsed == st::ParseResult::Newer ? StringId::ImportSettingsNewer
                                                     : StringId::ImportSettingsInvalid);
        return;
    }
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(StringId::ImportSettingsTitle);
    spec.message = Replace(Replace(l10n::Get(StringId::ImportSettingsPrompt), L"{version}",
                                   bundle.app_version.empty() ? L"?" : bundle.app_version),
                           L"{time}", st::DisplayTime(bundle.exported_at));
    spec.note = l10n::Get(StringId::ImportSettingsNote);
    spec.confirm_text = l10n::Get(StringId::ImportAndRestart);
    spec.secondary_text = l10n::Get(StringId::ImportRestartLater);
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xE8B5";  // Import
    const Choice choice = AskCategories(s, spec, bundle.categories);
    if (choice.button == ui::ConfirmChoice::Cancel || !choice.categories) return;
    app::SettingsTask task{app::SettingsTaskKind::SettingsImport};
    task.path = std::move(file);
    task.key = std::to_wstring(choice.categories);
    task.enabled = choice.button == ui::ConfirmChoice::Confirm;  // restart once staged
    Start(s, std::move(task), StringId::SettingsImportFailed);
}

void RestoreSettings(AppState& s) {
    const DiskState disk = Disk();
    if (disk.pending) {  // the button reads 立即重启 then
        RestartPulse(s);
        return;
    }
    if (!disk.has_backup) return;
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(StringId::RestoreSettingsTitle);
    spec.message = Replace(l10n::Get(StringId::RestoreSettingsPrompt), L"{time}", disk.backup_time);
    spec.confirm_text = l10n::Get(StringId::RestoreAndRestart);
    spec.tone = ui::ConfirmTone::Warning;
    spec.glyph = L"\xE777";  // UpdateRestore
    spec.default_choice = ui::ConfirmChoice::Cancel;
    if (Ask(s, spec) != ui::ConfirmChoice::Confirm) return;
    app::SettingsTask task{app::SettingsTaskKind::SettingsImport};
    task.path = disk.backup_dir;
    task.pin = true;      // a backup, staged as it is
    task.enabled = true;  // restart once staged
    Start(s, std::move(task), StringId::SettingsImportFailed);
}

} // namespace

SettingsTransferView GetSettingsTransferView(const AppState& s) {
    const DiskState disk = Disk();
    SettingsTransferView view;
    view.busy = s.settings.settings_transfer_pending();
    view.pending = disk.pending;
    view.has_backup = disk.has_backup;
    view.backup_time = disk.backup_time;
    return view;
}

void SettingsTransferAction(AppState& s, int action) {
    if (s.settings.settings_transfer_pending()) return;
    switch (action) {
    case 0: ExportSettings(s); break;
    case 1: ImportSettings(s); break;
    case 2: RestoreSettings(s); break;
    default: break;
    }
}

bool RunSettingsTransferTask(const app::SettingsTask& task, std::wstring& error) {
    const std::wstring dir = app::GetPulseDataDir();
    const uint32_t categories = static_cast<uint32_t>(wcstoul(task.key.c_str(), nullptr, 10));
    if (task.kind == app::SettingsTaskKind::SettingsExport) {
        std::wstring bundle;
        if (st::BuildBundle(dir, categories, {PULSE_VERSION_STRING, LocalStamp(true)}, bundle) &&
            WriteUtf8FileAtomic(task.path, bundle))
            return true;
        error = l10n::Get(StringId::SettingsExportFailed);
        return false;
    }
    bool staged = false;
    if (task.pin) {
        staged = st::StageRestore(dir, task.path);
    } else {
        std::wstring text;
        st::Bundle bundle;
        staged = ReadUtf8File(task.path, text) && st::ParseBundle(text, bundle) == st::ParseResult::Ok &&
                 st::StageImport(dir, bundle, categories);
    }
    if (!staged) error = l10n::Get(StringId::SettingsImportFailed);
    return staged;
}

void FinishSettingsTransfer(AppState& s, const app::SettingsTaskResult& result) {
    RefreshDisk();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (!result.ok) {
        Toast(s, result.error.empty() ? l10n::Get(StringId::SettingsImportFailed) : result.error);
        return;
    }
    if (result.task.kind == app::SettingsTaskKind::SettingsExport) {
        Toast(s, Replace(l10n::Get(StringId::SettingsExported), L"{file}", result.task.path));
        return;
    }
    if (result.task.enabled && RestartPulse(s)) return;
    Toast(s, l10n::Get(StringId::SettingsTransferPending));
}

bool RestartPulse(AppState& s) {
    const auto exe = app_runtime_paths::StableEntryPath();
    bool started = false;
    if (!exe.empty()) {
        // The new Pulse reopens this session's windows, as after an update.
        std::wstring command = L"\"" + exe + L"\" " +
                               kRestartAfterArgument + L" " + std::to_wstring(GetCurrentProcessId()) +
                               L" --restore-update-session";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(exe.data(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            started = true;
        }
    }
    if (!started) {
        Toast(s, l10n::Get(StringId::SettingsRestartFailed));
        return false;
    }
    PostMessageW(s.hwnd, WM_EXIT_PULSE, 0, 0);
    return true;
}

void WaitForRestartingPulse(unsigned long process_id) {
    if (!process_id || process_id == GetCurrentProcessId()) return;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (!process) return;  // already gone
    WaitForSingleObject(process, 30000);
    CloseHandle(process);
}

void ApplyStagedSettingsAtStartup() {
    const std::wstring dir = app::GetPulseDataDir();
    if (!st::HasPendingImport(dir)) return;
    g_startup = st::ApplyPendingImport(dir, LocalStamp(false));
}

void ShowSettingsTransferStartupToast(AppState& s) {
    const st::ApplyOutcome outcome = std::exchange(g_startup.outcome, st::ApplyOutcome::Nothing);
    switch (outcome) {
    case st::ApplyOutcome::Imported: Toast(s, l10n::Get(StringId::SettingsImportedToast)); break;
    case st::ApplyOutcome::Restored: Toast(s, l10n::Get(StringId::SettingsRestoredToast)); break;
    case st::ApplyOutcome::Failed: Toast(s, l10n::Get(StringId::SettingsApplyFailedToast)); break;
    case st::ApplyOutcome::Nothing: break;
    }
}

} // namespace pulse
