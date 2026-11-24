#include "workbench_adapter_host.h"
#include "../../app_navigation.h"
#include "../../desktop_preview_bridge.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../global_search_controller.h"
#include "../../places.h"
#include "../../tray_reveal.h"
#include "../../../fs/fs_snapshot.h"
#include "../../../ui/notification_toast.h"
#include "../../../ui/text_diff_window.h"
#include "../../../ui/window_effect_support.h"
#include "../../../common/localization.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace pulse::extensions::native {
namespace {

void EnsureHostWindowRevealed(AppState& app, HWND owner) noexcept {
    if (app.hidden_to_tray) {
        RevealWindow(app);
    } else if (IsIconic(owner)) {
        ShowWindow(owner, SW_RESTORE);
    }
    SetForegroundWindow(owner);
}

uint32_t PackAccentArgb(const D2D1_COLOR_F& color) noexcept {
    if (color.a <= 0.0f && color.r <= 0.0f && color.g <= 0.0f && color.b <= 0.0f) {
        return 0xFF0078D4u;
    }
    const auto to_byte = [](float v) noexcept -> uint32_t {
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    const uint32_t a = color.a > 0.0f ? to_byte(color.a) : 0xFFu;
    const uint32_t r = to_byte(color.r);
    const uint32_t g = to_byte(color.g);
    const uint32_t b = to_byte(color.b);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

PulseStatus NavigateOrRevealNormalizedPath(AppState& app, const std::wstring& raw_path, uint32_t mode) {
    const std::wstring normalized = fs::NormalizePath(raw_path);
    if (normalized.empty()) return PULSE_ERROR_ARGUMENT;
    if (mode == PULSE_NAVIGATE_CURRENT_TAB) {
        NavigateTo(app, normalized);
        return PULSE_OK;
    }
    if (mode == PULSE_NAVIGATE_NEW_TAB) {
        OpenFolderTab(app, normalized);
        return PULSE_OK;
    }
    if (mode == PULSE_NAVIGATE_REVEAL_ITEM) {
        std::filesystem::path p(normalized);
        if (!p.has_filename() || p == p.root_path()) {
            NavigateTo(app, normalized);
            return PULSE_OK;
        }
        const std::wstring parent = p.parent_path().wstring();
        const std::wstring name = p.filename().wstring();
        if (parent.empty() || name.empty()) return PULSE_ERROR_ARGUMENT;
        NavigateTo(app, parent);
        if (app.pane) {
            if (auto* tab = app.pane->ActiveTab()) {
                SelectNameInTab(app, *tab, name);
            }
        }
        return PULSE_OK;
    }
    return PULSE_ERROR_ARGUMENT;
}

ui::fluent::InfoBarKind MapInfoBarKind(uint32_t kind) noexcept {
    switch (kind) {
    case PULSE_NOTICE_SUCCESS: return ui::fluent::InfoBarKind::Success;
    case PULSE_NOTICE_WARNING: return ui::fluent::InfoBarKind::Warning;
    case PULSE_NOTICE_ERROR: return ui::fluent::InfoBarKind::Error;
    default: return ui::fluent::InfoBarKind::Informational;
    }
}

ui::SnackIcon MapSnackIcon(uint32_t kind) noexcept {
    switch (kind) {
    case PULSE_NOTICE_SUCCESS: return ui::SnackIcon::Merge;
    case PULSE_NOTICE_WARNING:
    case PULSE_NOTICE_ERROR: return ui::SnackIcon::Suggest;
    default: return ui::SnackIcon::Tab;
    }
}

std::vector<ui::SnackAction> BuildToastActions(const std::vector<WorkbenchActionSpec>& src) {
    const UINT msg = ExtensionWorkbenchActionMessage();
    std::vector<ui::SnackAction> out;
    out.reserve(src.size());
    for (const auto& item : src) {
        ui::SnackAction action;
        action.label = item.label;
        action.message = item.token != 0 ? msg : 0u;
        action.wparam = static_cast<WPARAM>(item.token);
        out.push_back(std::move(action));
    }
    return out;
}

PulseStatus WriteUnicodeClipboard(HWND owner, const std::wstring& text) noexcept {
    if (!OpenClipboard(owner)) return PULSE_ERROR_STATE;
    if (!EmptyClipboard()) {
        CloseClipboard();
        return PULSE_ERROR_STATE;
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return PULSE_ERROR_INITIALIZATION;
    }
    auto* dest = static_cast<wchar_t*>(GlobalLock(mem));
    if (!dest) {
        GlobalFree(mem);
        CloseClipboard();
        return PULSE_ERROR_INITIALIZATION;
    }
    if (!text.empty()) std::memcpy(dest, text.data(), text.size() * sizeof(wchar_t));
    dest[text.size()] = L'\0';
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return PULSE_ERROR_STATE;
    }
    CloseClipboard();
    return PULSE_OK;
}

} // namespace

UINT ExtensionWorkbenchActionMessage() noexcept {
    static const UINT msg = RegisterWindowMessageW(L"Pulse.Extension.WorkbenchAction.v1");
    return msg;
}

bool HandleWorkbenchActionMessage(HWND, UINT message, WPARAM wparam) noexcept {
    const UINT expected = ExtensionWorkbenchActionMessage();
    if (!expected || message != expected) return false;
    WorkbenchAdapter::DispatchHostActionToken(static_cast<uint64_t>(wparam));
    return true;
}

WorkbenchBackend MakeWorkbenchBackend(HWND owner) {
    WorkbenchBackend backend;
    backend.valid_owner = [owner] { return IsWindow(owner) != FALSE; };
    backend.show_snack = [owner](SnackPresentationSpec spec) -> bool {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return false;
        if ((spec.flags & PULSE_SNACK_FLAG_REPLACE) != 0) {
            app->notification_toast.ReplaceNextSnack();
        }
        app->notification_toast.ShowSnack(owner, MapSnackIcon(spec.kind), std::move(spec.text),
                                          std::move(spec.detail), BuildToastActions(spec.actions),
                                          {}, spec.duration_ms);
        return true;
    };
    backend.show_notice = [owner](NoticePresentationSpec spec) -> bool {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return false;
        app->notification_toast.ShowActionCard(owner, MapInfoBarKind(spec.kind), std::move(spec.title),
                                               std::move(spec.message), BuildToastActions(spec.actions));
        return true;
    };
    backend.reveal_path = [owner](const std::wstring& raw_path, uint32_t mode) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        return NavigateOrRevealNormalizedPath(*app, raw_path, mode);
    };
    backend.show_text_diff = [owner](const std::wstring& left_path, const std::wstring& right_path) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        if (!app->textDiff) app->textDiff = std::make_unique<ui::TextDiffWindow>();
        app->textDiff->Show(owner, fs::NormalizePath(left_path), fs::NormalizePath(right_path),
                            app->darkMode, app->accentColor);
        return PULSE_OK;
    };
    backend.set_clipboard_text = [owner](const std::wstring& text) -> PulseStatus {
        if (!IsWindow(owner)) return PULSE_ERROR_STATE;
        return WriteUnicodeClipboard(owner, text);
    };
    backend.open_search = [owner](const std::wstring& wide_query) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        // "pulse:quick-search:<text>" opens the quick-search window with the text, not a tab, and leaves
        // the main window where it is (the desktop pack sends what the user typed on the desktop).
        constexpr std::wstring_view kQuickSearch = L"pulse:quick-search:";
        if (wide_query.rfind(kQuickSearch, 0) == 0) {
            return ShowGlobalSearchWithQuery(*app, wide_query.substr(kQuickSearch.size())) ? PULSE_OK
                                                                                           : PULSE_ERROR_UNAVAILABLE;
        }
        EnsureHostWindowRevealed(*app, owner);
        std::wstring target = wide_query;
        if (target.rfind(L"pulse:search:", 0) != 0) {
            if (target.rfind(L"search:", 0) == 0) {
                target = L"pulse:" + target;
            } else {
                target = app::MakeSearchPath(target);
            }
        }
        OpenFolderTab(*app, target);
        InvalidateRect(owner, nullptr, FALSE);
        return PULSE_OK;
    };
    auto desktop_preview = std::make_shared<DesktopPreviewBridge>(owner);
    backend.reveal_path_from_rect = [owner, desktop_preview](const std::wstring& raw_path, uint32_t mode,
                                            const PulseScreenRect* origin_rect) -> PulseStatus {
        if (mode >= 3 && mode <= 5) {
            RECT rect{};
            if (origin_rect) rect = {origin_rect->x, origin_rect->y,
                static_cast<LONG>(std::clamp<int64_t>(int64_t(origin_rect->x) + origin_rect->width, INT32_MIN, INT32_MAX)),
                static_cast<LONG>(std::clamp<int64_t>(int64_t(origin_rect->y) + origin_rect->height, INT32_MIN, INT32_MAX))};
            return desktop_preview->Request(raw_path, mode, origin_rect ? &rect : nullptr) ? PULSE_OK : PULSE_ERROR_STATE;
        }
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        EnsureHostWindowRevealed(*app, owner);
        const PulseStatus status = NavigateOrRevealNormalizedPath(*app, raw_path, mode);
        if (status == PULSE_OK) {
            InvalidateRect(owner, nullptr, FALSE);
        }
        return status;
    };
    backend.query_appearance = [owner](PulseHostAppearance& out) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        out.size = sizeof(PulseHostAppearance);
        out.flags = app->darkMode ? PULSE_APPEARANCE_DARK : PULSE_APPEARANCE_LIGHT;
        out.window_effect = static_cast<uint32_t>(ui::WindowEffectFromId(app->appPrefs.window_effect));
        out.opacity_percent = static_cast<uint32_t>(std::clamp(100 - app->appPrefs.wallpaper_look, 30, 100));
        out.accent_argb = PackAccentArgb(app->accentColor);
        out.font_scale_milli = static_cast<uint32_t>(std::clamp(app->appPrefs.ui_font_scale, 50, 300) * 10);
        return PULSE_OK;
    };
    backend.query_locale = [owner](PulseHostLocale& out) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        out = {};
        out.size = sizeof(PulseHostLocale);
        const auto copy_tag = [](char (&dst)[32], const char* src) noexcept {
            const size_t len = std::min(std::strlen(src), sizeof(dst) - 1);
            std::memcpy(dst, src, len);
            dst[len] = '\0';
        };
        switch (l10n::effective_language()) {
        case l10n::Language::ZhTW: copy_tag(out.language_tag, "zh-TW"); break;
        case l10n::Language::EnUS: copy_tag(out.language_tag, "en-US"); break;
        case l10n::Language::ZhCN:
        case l10n::Language::System:
        default: copy_tag(out.language_tag, "zh-CN"); break;
        }
        const std::wstring& pref = app->appPrefs.language;
        if (pref.empty() || pref == L"system") {
            out.flags = PULSE_LOCALE_FOLLOW_SYSTEM;
            copy_tag(out.preference_tag, "system");
        } else if (pref == L"zh-TW") {
            copy_tag(out.preference_tag, "zh-TW");
        } else if (pref == L"en-US") {
            copy_tag(out.preference_tag, "en-US");
        } else {
            copy_tag(out.preference_tag, "zh-CN");
        }
        return PULSE_OK;
    };
    return backend;
}

WorkbenchAdapter::WorkbenchAdapter(HWND owner, std::function<bool()> alive,
                                   std::shared_ptr<DiagnosticsSink> diagnostics,
                                   std::function<void()> wake)
    : WorkbenchAdapter(MakeWorkbenchBackend(owner), std::move(alive), std::move(diagnostics), std::move(wake)) {}

} // namespace pulse::extensions::native
