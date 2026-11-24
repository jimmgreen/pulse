#include "vfs_adapter_host.h"
#include "../../app_navigation.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../tray_reveal.h"
#include "../vfs_path.h"

namespace pulse::extensions::native {

namespace {
std::wstring* g_open_capture = nullptr;
}

void CaptureVfsOpens(std::wstring* target) { g_open_capture = target; }

VfsBackend MakeVfsBackend(HWND owner) {
    VfsBackend backend;
    backend.valid_owner = [owner] { return IsWindow(owner) != FALSE; };
    backend.invalidate_path = [owner](std::string_view package_id,
                                      std::string_view mount_id,
                                      std::string_view rel_path_utf8) {
        auto* app = GetAppState(owner);
        if (!app) return;
        app::ForEachWindowSharingCore(*app, [&](AppState& window) {
            ForEachPane(window, [&](app::Pane& pane) {
                auto* tab = pane.ActiveTab();
                if (!tab) return;
                if (vfs::IsSameOrDescendantVfsPath(tab->current_path, package_id, mount_id, rel_path_utf8)) {
                    RefreshPath(window, tab->current_path, RefreshReason::Background);
                }
            });
        });
    };
    backend.open_path = [owner](const std::wstring& vfs_uri, uint32_t mode) -> PulseStatus {
        if (g_open_capture) {
            *g_open_capture = vfs_uri;
            return PULSE_OK;
        }
        auto* app = GetAppState(owner);
        if (!app) return PULSE_ERROR_STATE;
        if (app->hidden_to_tray) {
            RevealWindow(*app);
        } else if (IsIconic(owner)) {
            ShowWindow(owner, SW_RESTORE);
        }
        SetForegroundWindow(owner);
        if (mode == PULSE_VFS_OPEN_NEW_TAB) {
            OpenFolderTab(*app, vfs_uri);
            return PULSE_OK;
        }
        if (mode == PULSE_VFS_OPEN_CURRENT_TAB) {
            NavigateTo(*app, vfs_uri);
            return PULSE_OK;
        }
        return PULSE_ERROR_ARGUMENT;
    };
    return backend;
}

} // namespace pulse::extensions::native