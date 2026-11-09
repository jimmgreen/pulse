#pragma once
#include "../../../../sdk/native/include/pulse/ui.h"
#include "../../../../sdk/native/include/pulse/ui_events.h"
#include "../../../../sdk/native/include/pulse/ui_mount.h"
#include <d2d1.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <windows.h>
#include "../ui_tree.h"
#include <optional>
namespace pulse::extensions::native {
class UiAdapter {
  public:
    UiAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> alive);
    ~UiAdapter();
    UiAdapter(const UiAdapter &) = delete;
    UiAdapter &operator=(const UiAdapter &) = delete;
    PulseStatus QueryModel(uint32_t version, void *table, uint32_t size);
    PulseStatus QueryEvents(uint32_t version, void *table, uint32_t size);
    PulseStatus QueryData(uint32_t version, void *table, uint32_t size);
    PulseStatus QueryMount(uint32_t version, void *table, uint32_t size);
    // The dependent host calls keep standalone UI adapter users independent of
    // the full renderer. The AppState caller installs this before activation.
    template <class Host> void SyncHost(Host &host, std::function<void()> layout_changed = {}) {
        const HWND owner = host.hwnd;
        auto bounds = [target = &host, owner](uint32_t placement) -> RECT {
            if (!IsWindow(owner)) return {};
            const float width = static_cast<float>(target->compositor.Width());
            const float height = static_cast<float>(target->compositor.Height());
            const auto rect = placement == PULSE_UI_MOUNT_SIDEBAR
                ? target->renderer.ExtensionDockRect(width, height) : target->renderer.ContentRect(width, height);
            return {static_cast<LONG>(rect.left), static_cast<LONG>(rect.top),
                static_cast<LONG>(rect.right), static_cast<LONG>(rect.bottom)};
        };
        auto sidebar = [target = &host, owner, layout_changed = std::move(layout_changed)](bool visible) {
            if (!IsWindow(owner)) return;
            target->renderer.SetExtensionDockVisible(visible);
            if (layout_changed) layout_changed();
            InvalidateRect(owner, nullptr, FALSE);
        };
        SyncHostLayout(owner, host.darkMode, host.accentColor, host.scale, std::move(bounds), std::move(sidebar));
    }
    bool IsSidebar() const;
    bool IsWorkspace() const;
    static bool SidebarOccupied(HWND owner);
    static void SetLegacySidebar(HWND owner, bool occupied);
    bool Embed();
    bool ShowEmbedded(RECT bounds, bool dark, D2D1_COLOR_F accent, float scale);
    void HideEmbedded();
    bool HasPanel(uint64_t id) const;
    void Stop();
    bool InCallback() const;
    HWND NodeWindow(const std::string &id) const;
    HWND Window() const;
    std::optional<UiNode> DataNode(const std::string &id) const;
    bool Snapshot(const std::wstring &path, bool dark, float scale, const std::string &reveal = {});
    bool SnapshotCurrent(const std::wstring &path);
    PulseUiMountInfo MountInfo() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
    void SyncHostLayout(HWND owner, bool dark, D2D1_COLOR_F accent, float scale,
                        std::function<RECT(uint32_t)> bounds, std::function<void(bool)> sidebar);
};
} // namespace pulse::extensions::native
