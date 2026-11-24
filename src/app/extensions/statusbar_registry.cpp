#include "statusbar_registry.h"

#include <algorithm>
#include <utility>

namespace pulse::extensions::statusbar {

Registry& Registry::Instance() {
    static Registry registry;
    return registry;
}

void Registry::Changed() {
    ++revision_;
    if (changed_) {
        try { changed_(); } catch (...) {}
    }
}

uint64_t Registry::Add(Item item) {
    if (item.handle == 0) item.handle = next_handle_++;
    const uint64_t handle = item.handle;
    items_.push_back(std::move(item));
    Changed();
    return handle;
}

bool Registry::Update(uint64_t handle, const Item& fields) {
    for (auto& item : items_) {
        if (item.handle != handle) continue;
        item.text = fields.text;
        item.tooltip = fields.tooltip;
        item.icon = fields.icon;
        item.tone = fields.tone;
        item.click = fields.click;
        Changed();
        return true;
    }
    return false;
}

bool Registry::SetVisible(uint64_t handle, bool visible) {
    for (auto& item : items_) {
        if (item.handle != handle) continue;
        if (item.visible != visible) {
            item.visible = visible;
            Changed();
        }
        return true;
    }
    return false;
}

bool Registry::Remove(uint64_t handle) {
    const auto it = std::find_if(items_.begin(), items_.end(), [&](const Item& item) { return item.handle == handle; });
    if (it == items_.end()) return false;
    items_.erase(it);
    Changed();
    return true;
}

void Registry::RemoveOwner(uint64_t owner) {
    const auto removed = std::erase_if(items_, [&](const Item& item) { return item.owner == owner; });
    if (removed) Changed();
}

size_t Registry::CountForPackage(const std::string& package_id) const {
    return static_cast<size_t>(std::count_if(items_.begin(), items_.end(),
        [&](const Item& item) { return item.package_id == package_id; }));
}

bool Registry::HasId(const std::string& package_id, const std::string& id) const {
    return std::any_of(items_.begin(), items_.end(),
        [&](const Item& item) { return item.package_id == package_id && item.id == id; });
}

const Item* Registry::Find(uint64_t handle) const {
    for (const auto& item : items_)
        if (item.handle == handle) return &item;
    return nullptr;
}

std::vector<View> Registry::Visible() const {
    std::vector<View> out;
    for (const auto& item : items_) {
        if (!item.visible) continue;
        out.push_back({item.text, item.tooltip, item.icon, item.tone, static_cast<bool>(item.click), item.handle});
    }
    return out;
}

bool Registry::VisibleClickable(int index) const {
    const auto views = Visible();
    return index >= 0 && static_cast<size_t>(index) < views.size() && views[static_cast<size_t>(index)].clickable;
}

bool Registry::Click(uint64_t handle) {
    const Item* item = Find(handle);
    if (!item || !item->visible || !item->click) return false;
    // A copy: the callback may update or remove its own item.
    const auto click = item->click;
    try { click(); } catch (...) {}
    return true;
}

void Registry::SetChangedCallback(std::function<void()> changed) { changed_ = std::move(changed); }

} // namespace pulse::extensions::statusbar
