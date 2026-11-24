#include "dialog_classifier.h"

#include <cwchar>
#include <string_view>

namespace pulse::dialog_overlay {
namespace {

constexpr int kCtrlFilenameEditPrimary = 1148;   // cmb13 / edt1 in Vista+ & legacy Explorer dialogs
constexpr int kCtrlFilenameEditLegacy  = 1152;   // edt1 in older template dialogs
constexpr int kCtrlOkButton            = IDOK;   // 1
constexpr int kCtrlCancelButton        = IDCANCEL; // 2

std::wstring GetClassText(HWND hwnd) {
    wchar_t buf[256]{};
    const int n = GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return n > 0 ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring{};
}

std::wstring GetWindowTitleText(HWND hwnd) {
    wchar_t buf[512]{};
    const int n = GetWindowTextW(hwnd, buf, ARRAYSIZE(buf));
    return n > 0 ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring{};
}

bool EqualsCi(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ProcessBasename(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[32768]{};
    DWORD len = ARRAYSIZE(buf);
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) {
        std::wstring_view view(buf, len);
        const auto slash = view.find_last_of(L"\\/");
        result = std::wstring(slash == std::wstring_view::npos ? view : view.substr(slash + 1));
    }
    CloseHandle(proc);
    return result;
}

struct ChildScanState {
    bool has_dui_view = false;
    bool has_direct_ui = false;
    bool has_shelldll_defview = false;
    bool has_tree_view = false;
    bool has_ctrl_notify_sink_custom = false;
    HWND filename_edit = nullptr;
    HWND ok_button = nullptr;
    HWND cancel_button = nullptr;
    HWND root_dialog = nullptr;
};

BOOL CALLBACK ScanChildProc(HWND child, LPARAM lparam) {
    auto* st = reinterpret_cast<ChildScanState*>(lparam);
    const std::wstring cls = GetClassText(child);
    const int ctrl_id = GetDlgCtrlID(child);

    if (EqualsCi(cls, L"DUIViewWndClassName")) {
        st->has_dui_view = true;
    } else if (EqualsCi(cls, L"DirectUIHWND")) {
        st->has_direct_ui = true;
    } else if (EqualsCi(cls, L"SHELLDLL_DefView")) {
        st->has_shelldll_defview = true;
    } else if (EqualsCi(cls, L"SysTreeView32") || EqualsCi(cls, L"SHBrowseForFolder ShellNameSpace Control")) {
        st->has_tree_view = true;
    } else if (EqualsCi(cls, L"CtrlNotifySink")) {
        // Standard Vista dialogs create CtrlNotifySink containers for host-added
        // IFileDialogCustomize controls (check boxes, combo boxes, edit boxes).
        // Check if this sink actually hosts a visible non-empty custom control.
        HWND inner = GetWindow(child, GW_CHILD);
        while (inner) {
            const std::wstring inner_cls = GetClassText(inner);
            if (!EqualsCi(inner_cls, L"ScrollBar") && IsWindowVisible(inner)) {
                const int inner_id = GetDlgCtrlID(inner);
                // Ignore standard filename combo (1148), filter combo (1136), and standard buttons (1/2).
                if (inner_id != kCtrlFilenameEditPrimary && inner_id != kCtrlFilenameEditLegacy &&
                    inner_id != 1136 && inner_id != kCtrlOkButton && inner_id != kCtrlCancelButton) {
                    st->has_ctrl_notify_sink_custom = true;
                }
            }
            inner = GetWindow(inner, GW_HWNDNEXT);
        }
    }

    if (EqualsCi(cls, L"Edit")) {
        HWND parent = GetParent(child);
        const int parent_id = parent ? GetDlgCtrlID(parent) : 0;
        HWND grand = parent ? GetParent(parent) : nullptr;
        const int grand_id = grand ? GetDlgCtrlID(grand) : 0;
        if (ctrl_id == kCtrlFilenameEditPrimary || parent_id == kCtrlFilenameEditPrimary || grand_id == kCtrlFilenameEditPrimary) {
            st->filename_edit = child;
        } else if (!st->filename_edit && (ctrl_id == kCtrlFilenameEditLegacy || parent_id == kCtrlFilenameEditLegacy)) {
            st->filename_edit = child;
        }
    } else if (EqualsCi(cls, L"Button")) {
        if (ctrl_id == kCtrlOkButton && !st->ok_button) st->ok_button = child;
        else if (ctrl_id == kCtrlCancelButton && !st->cancel_button) st->cancel_button = child;
    }

    return TRUE;
}

} // namespace

NativeDialogInfo ClassifyNativeDialog(HWND hwnd,
                                      const dialog_bridge_state::ActiveDialogLedger* ledger) noexcept {
    NativeDialogInfo info{};
    if (!hwnd || !IsWindow(hwnd)) return info;

    info.dialog_hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &info.owner_pid);
    if (info.owner_pid == 0 || info.owner_pid == GetCurrentProcessId()) return info;

    // Layer 1/2 deduplication: never double-take over a process already owned by Pulse COM/SinkHooks.
    if (ledger && ledger->IsTakenOver(info.owner_pid)) {
        info.kind = NativeDialogKind::AlreadyTakenOverByPulse;
        return info;
    }

    info.window_class = GetClassText(hwnd);
    info.title = GetWindowTitleText(hwnd);

    const std::wstring exe = ProcessBasename(info.owner_pid);
    if (EqualsCi(exe, L"PickerHost.exe")) {
        info.kind = NativeDialogKind::BrokeredPickerHost;
        // Brokered UWP PickerHost dialogs are strictly Mode A (companion jump bar).
        info.mode_b_eligible = false;
        return info;
    }

    if (!EqualsCi(info.window_class, L"#32770")) {
        return info;
    }

    ChildScanState scan{};
    scan.root_dialog = hwnd;
    EnumChildWindows(hwnd, &ScanChildProc, reinterpret_cast<LPARAM>(&scan));

    info.filename_edit_hwnd = scan.filename_edit;
    info.ok_button_hwnd = scan.ok_button;
    info.cancel_button_hwnd = scan.cancel_button;
    info.has_custom_controls = scan.has_ctrl_notify_sink_custom;

    if (scan.has_dui_view && scan.has_direct_ui && scan.filename_edit && scan.ok_button) {
        info.kind = NativeDialogKind::VistaFileDialog;
        info.mode_b_eligible = !info.has_custom_controls;
    } else if (scan.has_shelldll_defview && scan.filename_edit && scan.ok_button) {
        info.kind = NativeDialogKind::LegacyFileDialog;
        info.mode_b_eligible = !info.has_custom_controls;
    } else if (scan.has_tree_view && scan.ok_button) {
        info.kind = NativeDialogKind::BrowseForFolderDialog;
        info.mode_b_eligible = false;
    } else if (scan.filename_edit && scan.ok_button && scan.cancel_button) {
        // Minimal/test #32770 file dialog template with standard Edit(1148) + IDOK + IDCANCEL.
        info.kind = NativeDialogKind::LegacyFileDialog;
        info.mode_b_eligible = !info.has_custom_controls;
    }

    return info;
}

bool DriveNativeDialogNavigation(const NativeDialogInfo& info, const std::wstring& path_or_folder,
                                 bool click_ok) noexcept {
    if (!info.dialog_hwnd || !IsWindow(info.dialog_hwnd) ||
        !info.filename_edit_hwnd || !IsWindow(info.filename_edit_hwnd) ||
        path_or_folder.empty()) {
        return false;
    }

    // Use SendMessageTimeoutW so a hung target dialog can never freeze the overlay!
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(info.filename_edit_hwnd, WM_SETTEXT, 0,
                             reinterpret_cast<LPARAM>(path_or_folder.c_str()),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result)) {
        return false;
    }

    if (click_ok) {
        if (info.ok_button_hwnd && IsWindow(info.ok_button_hwnd)) {
            if (!SendMessageTimeoutW(info.dialog_hwnd, WM_COMMAND,
                                     MAKEWPARAM(kCtrlOkButton, BN_CLICKED),
                                     reinterpret_cast<LPARAM>(info.ok_button_hwnd),
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result)) {
                return false;
            }
        } else {
            PostMessageW(info.filename_edit_hwnd, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageW(info.filename_edit_hwnd, WM_KEYUP, VK_RETURN, 0);
        }
    }
    return true;
}

bool CancelNativeDialog(const NativeDialogInfo& info) noexcept {
    if (!info.dialog_hwnd || !IsWindow(info.dialog_hwnd)) return false;
    DWORD_PTR result = 0;
    if (info.cancel_button_hwnd && IsWindow(info.cancel_button_hwnd)) {
        return SendMessageTimeoutW(info.dialog_hwnd, WM_COMMAND,
                                   MAKEWPARAM(kCtrlCancelButton, BN_CLICKED),
                                   reinterpret_cast<LPARAM>(info.cancel_button_hwnd),
                                   SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result) != FALSE;
    }
    return PostMessageW(info.dialog_hwnd, WM_CLOSE, 0, 0) != FALSE;
}

} // namespace pulse::dialog_overlay
