#include "global_search_controller.h"
#include "app_internal.h"
#include "../common/localization.h"

namespace pulse {
void ApplyGlobalSearchSettings(AppState& s) {
    // Only the primary window owns the global hotkey and the tray icon; a second
    // window started with --new-window must not fight it for either.
    if (s.isolatedTest || s.secondaryInstance) return;
    const bool enabled = s.appPrefs.global_search_enabled;
    const bool ok = s.globalSearchHotkey.Update(s.hwnd, enabled,
        s.appPrefs.global_search_modifiers, s.appPrefs.global_search_key);
    s.settings.SetGlobalSearchError(ok ? std::wstring{} : l10n::Get(
        s.globalSearchHotkey.Error() == ERROR_HOTKEY_ALREADY_REGISTERED
            ? l10n::StringId::GlobalSearchConflict : l10n::StringId::GlobalSearchRegisterFailed));
    if (!enabled || !ok) s.globalSearchWindow.Hide();
    s.tray_controller.SetVisible(s.appPrefs.keep_running_on_close || enabled);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key) {
    if (!s.settings.global_search_hotkey_capturing()) return false;
    if (!IsSettingsTab(ActiveTab(s))) {
        s.settings.CancelGlobalSearchHotkeyCapture();
        return false;
    }
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
    const bool consumed = s.settings.CaptureGlobalSearchHotkey(key, modifiers);
    if (consumed) InvalidateRect(s.hwnd, nullptr, FALSE);
    return consumed;
}

void ToggleGlobalSearch(AppState& s) {
    if (!s.appPrefs.global_search_enabled || !s.globalSearchHotkey.Registered()) return;
    if (s.globalSearchWindow.Visible()) {
        s.globalSearchWindow.Hide();
        return;
    }
    std::wstring folder;
    s.globalSearchWindow.SetAppearance(s.darkMode, ui::WindowEffectFromId(s.appPrefs.window_effect),
        s.appPrefs.background_image, s.accentColor);
    if (const auto* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path)) folder = tab->current_path;
    if (!s.globalSearchWindow.Show(s.hwnd, s.darkMode, s.scale, folder, s.appPrefs.search_pinyin)) {
        s.tray_controller.RestoreWindow();
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::GlobalSearch),
            l10n::Get(l10n::StringId::GlobalSearchOpenFailed));
    }
}

void ShutdownGlobalSearch(AppState& s) {
    s.globalSearchHotkey.Reset();
    s.globalSearchWindow.Shutdown();
}
}
