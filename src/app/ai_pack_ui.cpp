#include "ai_pack_ui.h"
#include "ai_pack.h"
#include "pack_installer.h"
#include "pack_failure_ui.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../common/preview_packs.h"
#include "../common/runtime_log.h"
#include "../common/text_format.h"
#include "../ui/confirm_dialog.h"

namespace pulse {
namespace {

using l10n::StringId;

const std::wstring& Text(StringId id) { return l10n::Get(id); }

std::wstring Fill(std::wstring text, const std::wstring& value) {
    const size_t at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, value);
    return text;
}

std::wstring DownloadSize() {
    return format::ByteSize(app::ContentPackDownloadBytes(app::AiPackRelease()));
}

// One download per process, whichever window asked. Never destroyed: a
// download still running at exit just ends with the process.
app::PackInstaller& Installer() {
    static auto* installer = new app::PackInstaller;
    return *installer;
}

HWND g_owner = nullptr;   // window that reports the outcome, null when idle
bool g_silent = false;    // background update: no toasts

bool StartInstall(AppState& s, bool silent) {
    if (Installer().running()) return true;
    if (!Installer().Start(app::AiPackRelease(), s.hwnd)) return false;
    g_owner = s.hwnd;
    g_silent = silent;
    diagnostics::runtime::Event("ai_pack_install_started", {{"silent", static_cast<uint64_t>(silent)}});
    return true;
}

bool AskInstall(AppState& s, app::AiPackState state) {
    ui::ConfirmDialogSpec spec;
    const std::wstring size = DownloadSize();
    if (state == app::AiPackState::Outdated) {
        spec.title = Text(StringId::AiPackUpdateTitle);
        spec.message = Fill(Text(StringId::AiPackUpdateMessage), size);
    } else if (state == app::AiPackState::Damaged) {
        spec.title = Text(StringId::AiPackRepairTitle);
        spec.message = Fill(Text(StringId::AiPackRepairMessage), size);
    } else {
        spec.title = Text(StringId::AiPackAskTitle);
        spec.message = Fill(Text(StringId::AiPackAskMessage), size);
    }
    spec.items = {Text(StringId::AiPackAskItem)};
    spec.item_glyph = L"\xE99A";  // Robot
    spec.note = Text(StringId::AiPackAskNote);
    spec.confirm_text = Text(StringId::DownloadAndInstall);
    spec.cancel_text = Text(StringId::AiPackNotNow);
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xE896";  // Download
    spec.default_choice = ui::ConfirmChoice::Confirm;
    return ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) == ui::ConfirmChoice::Confirm;
}

// After Pulse was updated: replace an installed pack quietly when the user
// lets packs update themselves. Only installed.json is read here, never the
// file, so this costs nothing at startup.
void MaybeUpdateInBackground(AppState& s) {
    const auto& release = app::AiPackRelease();
    if (!release.file_count) return;
    const std::wstring root = packs::PacksRoot();
    if (root.empty()) return;
    std::wstring version;
    if (packs::InstalledPackDirectory(root + L"\\" + release.key, &version).empty()) return;
    if (version == release.version || !packs::LoadPackSettings().auto_update) return;
    if (app::QueryAiPackState() != app::AiPackState::Outdated) return;  // e.g. Bundled
    StartInstall(s, true);
}

} // namespace

void OfferAiPack(AppState& s) {
    const auto state = app::QueryAiPackState();
    if (!app::AiPackInstallable(state) || s.isolatedTest || Installer().running()) return;
    if (AskInstall(s, state) && !StartInstall(s, false))
        s.notification_toast.ShowError(s.hwnd, Text(StringId::AiPackToastTitle), Text(StringId::AiPackStartFailed));
}

void CancelAiPackInstall() { Installer().Cancel(); }

bool ConfirmRemoveAiPack(AppState& s) {
    if (Installer().running() || s.isolatedTest) return false;
    ui::ConfirmDialogSpec spec;
    spec.title = Text(StringId::AiPackRemoveTitle);
    spec.message = Text(StringId::AiPackRemoveMessage);
    spec.confirm_text = Text(StringId::Remove_2);
    spec.cancel_text = Text(StringId::Cancel);
    spec.tone = ui::ConfirmTone::Warning;
    spec.glyph = L"\xE74D";  // Delete
    spec.default_choice = ui::ConfirmChoice::Cancel;
    if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return false;
    DWORD error = 0;
    const bool removed = app::RemoveAiPack(error);
    diagnostics::runtime::Event("ai_pack_removed", {{"ok", static_cast<uint64_t>(removed)},
                                                    {"error", static_cast<uint64_t>(error)}});
    if (!removed) {
        s.notification_toast.ShowError(s.hwnd, Text(StringId::AiPackToastTitle), Text(StringId::AiPackRemoveFailed));
        return false;
    }
    s.notification_toast.SetKind(ui::fluent::InfoBarKind::Success);
    s.notification_toast.Show(s.hwnd, Text(StringId::AiPackRemoved), std::wstring{}, false);
    return true;
}

bool AiPackInstalling() noexcept { return Installer().running(); }
float AiPackInstallProgress() noexcept { return Installer().progress(); }

void TickAiPackInstall(AppState& s) {
    static bool checked_update = false;
    if (!checked_update && !s.isolatedTest && s.hwnd) {
        checked_update = true;
        MaybeUpdateInBackground(s);
    }
    // Screenshot fixture: the failure card of PULSE_TEST_PACK_FAILURE (a
    // PackFailure value), once, in an isolated test instance.
    static bool fixture_shown = false;
    wchar_t fixture[8]{};
    if (!fixture_shown && s.isolatedTest && s.hwnd &&
        GetEnvironmentVariableW(L"PULSE_TEST_PACK_FAILURE", fixture, ARRAYSIZE(fixture))) {
        fixture_shown = true;
        const auto failure = static_cast<app::PackFailure>(_wtoi(fixture));
        const DWORD error = failure == app::PackFailure::Network ? 12029u
                          : failure == app::PackFailure::Mismatch ? static_cast<DWORD>(ERROR_FILE_TOO_LARGE)
                          : failure == app::PackFailure::Disk ? static_cast<DWORD>(ERROR_DISK_FULL) : 1u;
        ShowPackFailure(s, FailedPack::Ai, failure, error);
    }
    if (!g_owner || Installer().running()) return;
    // The window that asked reports the outcome, unless it was closed.
    if (s.hwnd != g_owner && IsWindow(g_owner)) return;
    DWORD error = 0;
    app::PackFailure failure = app::PackFailure::None;
    const auto outcome = Installer().TakeOutcome(error, &failure);
    if (outcome == app::PackInstallOutcome::None) return;
    const bool silent = g_silent;
    g_owner = nullptr;
    diagnostics::runtime::Event("ai_pack_install_done",
                                {{"outcome", static_cast<uint64_t>(outcome)}, {"error", static_cast<uint64_t>(error)},
                                 {"failure", static_cast<uint64_t>(failure)}});
    if (outcome == app::PackInstallOutcome::Installed) {
        app::ForgetVerifiedAiPack();
        if (silent) return;
        s.notification_toast.SetKind(ui::fluent::InfoBarKind::Success);
        s.notification_toast.Show(s.hwnd, Text(StringId::AiPackInstalledTitle), Text(StringId::AiPackInstalledMessage),
                                  false);
    } else if (outcome == app::PackInstallOutcome::Failed && !silent) {
        ShowPackFailure(s, FailedPack::Ai, failure, error);
    }
}

bool RetryAiPackInstall(AppState& s) {
    const bool started = StartInstall(s, false);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return started;
}

} // namespace pulse
