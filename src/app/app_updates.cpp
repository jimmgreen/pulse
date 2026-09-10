#include "app_updates.h"
#include "app_state.h"
#include "../common/localization.h"
#include <cstdio>

namespace pulse {
namespace {
constexpr ULONGLONG kCheckInterval = 6ull * 60 * 60 * 1000;
void ShowInstallError(AppState& state) {
    using l10n::StringId;
    const auto message = state.update_install_error == ERROR_CANCELLED ? StringId::UpdateCancelled :
        state.update_install_error == ERROR_BUSY ? StringId::UpdateBusy : StringId::UpdateInstallFailed;
    state.notification_toast.Show(state.hwnd, l10n::Get(StringId::Update), l10n::Get(message));
}
}

void CheckForUpdates(AppState& state) {
    if (state.update_installer.downloading() || state.update_installer.installing()) return;
    if (state.update_checker.CheckAsync(state.hwnd, WM_UPDATE_RESULT)) {
        state.update_result_ready = false;
        state.update_install_error = ERROR_SUCCESS;
        state.next_update_check = GetTickCount64() + kCheckInterval;
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
}

void TickUpdates(AppState& state, unsigned long long now) {
    DWORD install_error = ERROR_SUCCESS;
    if (state.update_installer.TakeInstallResult(install_error)) {
        state.update_install_error = install_error;
        if (install_error) ShowInstallError(state);
        state.update_installer.Stop();
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
    if (state.shot.active || !app::UpdateChecker::Enabled() || now < state.next_update_check) return;
    if (state.update_installer.downloading() || state.update_installer.installing() || state.update_checker.checking()) return;
    state.next_update_check = now + kCheckInterval;
    CheckForUpdates(state);
}

void InstallUpdate(AppState& state) {
    if (state.update_installer.installing()) return;
    if (state.update_installer.downloading()) {
        state.update_installer.Stop();
        state.update_install_error = ERROR_CANCELLED;
    } else if (state.update_result_ready && state.update_result.update_available) {
        state.update_install_error = ERROR_SUCCESS;
        if (!state.update_installer.Start(state.update_result, state.hwnd, WM_UPDATE_DOWNLOADED))
            state.update_install_error = ERROR_GEN_FAILURE;
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CompleteUpdateCheck(AppState& state) {
    app::UpdateResult result;
    if (!state.update_checker.TakeResult(result)) return;
    state.update_result = std::move(result);
    state.update_result_ready = true;
    if (state.update_result.update_available && state.notified_update_version != state.update_result.version) {
        state.notified_update_version = state.update_result.version;
        wchar_t title[160]{};
        swprintf_s(title, l10n::Get(l10n::StringId::UpdateAvailableFormat).c_str(),
            state.update_result.version.c_str());
        state.notification_toast.Show(state.hwnd, title,
            l10n::Get(l10n::StringId::UpdateClickToInstall), true, WM_UPDATE_INSTALL);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CompleteUpdateDownload(AppState& state) {
    DWORD error = ERROR_SUCCESS;
    if (!state.update_installer.TakeResult(error)) return;
    if (!error && (state.ops.Status().active || state.settings.migration_pending())) error = ERROR_BUSY;
    if (!error) state.update_installer.Launch(state.hwnd, error);
    state.update_install_error = error;
    if (error) {
        state.update_installer.Stop();
        ShowInstallError(state);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}
}
