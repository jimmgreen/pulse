#include "package_lifecycle.h"
#include "../runtime.h"
#include "session.h"
#include "../resource_service.h"
#include "../transfer_ui.h"
#include "../delete_ui.h"
#include "../authorization.h"
#include "../../app_runtime.h"
#include "../../window_registry.h"
#include <set>
#include <algorithm>

namespace pulse::extensions::native {
namespace {
constexpr UINT_PTR kDevelopmentTimer = 0x455844;
std::set<HWND> monitoring_owners;
}
bool StartPackageMonitoring(HWND window) {
    if (!PackageStoreMode() && !DevelopmentMode()) return true;
    if (monitoring_owners.contains(window)) return true;
    if (!SetTimer(window, kDevelopmentTimer, 100, nullptr)) return false;
    if (!StartDevelopmentReload()) { KillTimer(window, kDevelopmentTimer); return false; }
    monitoring_owners.insert(window);
    return true;
}
bool HandlePackageMonitoringTimer(HWND window, WPARAM timer, AppState& state) {
    if (!monitoring_owners.contains(window) || timer != kDevelopmentTimer) return false;
    if (std::any_of(state.appPrefs.toolbar_tools.begin(), state.appPrefs.toolbar_tools.end(), [](const auto& id) { return id.starts_with(L"native:"); }))
        PrepareCommandPackages(state);
    if (PollDevelopmentReload([&] {
        app::ForEachWindowSharingCore(state, [](AppState& window) {
            if (window.menu && window.menu->IsOpen()) window.menu->Dismiss();
        });
    })) InvalidateRect(window, nullptr, FALSE);
    return true;
}
void StopPackageMonitoring(HWND window) {
    if (!monitoring_owners.erase(window)) return;
    KillTimer(window, kDevelopmentTimer);
    if (monitoring_owners.empty()) StopDevelopmentReload();
}
void ReleasePackageOwner(HWND owner) {
    StopOwner(owner);
    ResourceService::ReleaseOwner(reinterpret_cast<uintptr_t>(owner));
    CancelTransferUi(owner);
    CancelDeleteUi(owner);
    ReleaseCallerOwner(reinterpret_cast<uintptr_t>(owner));
}
}
