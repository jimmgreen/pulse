#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::extensions::statusbar {

// One plugin status-bar item (pulse.statusbar). The adapter fills it after validation.
struct Item {
    uint64_t handle = 0;
    uint64_t owner = 0;          // adapter instance; RemoveOwner drops all of its items
    std::string package_id, id;
    std::wstring text, tooltip;
    uint32_t icon = 0;           // resolved icon registry id; 0 = none
    uint32_t tone = 0;           // PULSE_STATUS_TONE_*
    bool visible = true;
    std::function<void()> click; // empty: a plain label
};

// What the status bar draws, in registration order (visible items only).
struct View {
    std::wstring text, tooltip;
    uint32_t icon = 0, tone = 0;
    bool clickable = false;
    uint64_t handle = 0;
};

// Process-wide list shared by every plugin session. UI thread only.
class Registry {
public:
    static Registry& Instance();

    uint64_t NewHandle() noexcept { return next_handle_++; }
    uint64_t Add(Item item);  // uses item.handle when NewHandle reserved one
    bool Update(uint64_t handle, const Item& fields); // keeps handle, owner, package and id
    bool SetVisible(uint64_t handle, bool visible);
    bool Remove(uint64_t handle);
    void RemoveOwner(uint64_t owner);
    size_t CountForPackage(const std::string& package_id) const;
    bool HasId(const std::string& package_id, const std::string& id) const;
    const Item* Find(uint64_t handle) const;

    std::vector<View> Visible() const;
    // Runs the click of a visible item by handle; false when it has none or is gone.
    bool Click(uint64_t handle);
    bool VisibleClickable(int index) const;  // index into Visible()

    // Called after any change (the app repaints the status bar).
    void SetChangedCallback(std::function<void()> changed);
    uint64_t Revision() const noexcept { return revision_; }

private:
    void Changed();
    std::vector<Item> items_;
    uint64_t next_handle_ = 1;
    uint64_t revision_ = 0;
    std::function<void()> changed_;
};

} // namespace pulse::extensions::statusbar
