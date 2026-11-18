// Display-only extension store contract. App owns fetching, authorization and installation.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {
struct ExtensionStoreItem {
    std::wstring name, summary, publisher, version, status, button_label;
    bool action_enabled = false;
    bool official = false; // listed under "Pulse official" instead of the community section
    std::wstring icon_glyph, size; // size: "1.0 MB"
    uint32_t icon_color = 0x527D70; // 0xRRGGBB tile color
};
// Registry snapshot, including disabled packages; never derived from the running runtime.
struct InstalledExtensionItem {
    std::wstring name, id, version, publisher, status, notice;
    std::string revision; // manifest SHA-256 of the active version
    bool enabled = false;
    bool native_code = false;
    bool can_enable = false, can_disable = false, can_uninstall = false, can_rollback = false;
};
struct ExtensionStoreView {
    enum class State { Idle, Loading, Failed, Offline, Online };
    State state = State::Idle;
    std::wstring notice;
    bool pending_restart = false;
    bool busy = false; // Shows StoreCancel; app decides whether cancellation is available.
    std::vector<ExtensionStoreItem> items;
    std::vector<InstalledExtensionItem> installed;
    bool installed_loading = false;
    bool installed_busy = false; // Local mutation/review only; independent of catalog network loading.
    std::wstring installed_error;
};
} // namespace pulse::ui
