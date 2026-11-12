#include "workbench_adapter_host.h"
#include "../../app_navigation.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../../fs/fs_snapshot.h"
#include "../../../ui/notification_toast.h"
#include "../../../ui/text_diff_window.h"
#include <cstring>
#include <filesystem>

namespace pulse::extensions::native {
namespace {

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
        const std::wstring normalized = fs::NormalizePath(raw_path);
        if (normalized.empty()) return PULSE_ERROR_ARGUMENT;
        if (mode == PULSE_NAVIGATE_CURRENT_TAB) {
            NavigateTo(*app, normalized);
            return PULSE_OK;
        }
        if (mode == PULSE_NAVIGATE_NEW_TAB) {
            OpenFolderTab(*app, normalized);
            return PULSE_OK;
        }
        if (mode == PULSE_NAVIGATE_REVEAL_ITEM) {
            std::filesystem::path p(normalized);
            if (!p.has_filename() || p == p.root_path()) {
                NavigateTo(*app, normalized);
                return PULSE_OK;
            }
            const std::wstring parent = p.parent_path().wstring();
            const std::wstring name = p.filename().wstring();
            if (parent.empty() || name.empty()) return PULSE_ERROR_ARGUMENT;
            NavigateTo(*app, parent);
            if (app->pane) {
                if (auto* tab = app->pane->ActiveTab()) {
                    SelectNameInTab(*app, *tab, name);
                }
            }
            return PULSE_OK;
        }
        return PULSE_ERROR_ARGUMENT;
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
    return backend;
}

WorkbenchAdapter::WorkbenchAdapter(HWND owner, std::function<bool()> alive,
                                   std::shared_ptr<DiagnosticsSink> diagnostics)
    : WorkbenchAdapter(MakeWorkbenchBackend(owner), std::move(alive), std::move(diagnostics)) {}

} // namespace pulse::extensions::native
