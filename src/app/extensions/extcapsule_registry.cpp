#include "extcapsule_registry.h"

#include <algorithm>
#include <utility>

namespace pulse::extensions::extcapsules {

Registry::Registry(bool publish_to_ui) : publish_to_ui_(publish_to_ui) {}

Registry& Registry::Instance() {
    static Registry registry(true);
    return registry;
}

void Registry::SetRules(uint64_t owner, std::string package_id, std::vector<Entry> entries) {
    if (entries.empty()) {
        RemoveOwner(owner);
        return;
    }
    for (auto& existing : owners_) {
        if (existing.owner == owner) {
            existing.package_id = std::move(package_id);
            existing.entries = std::move(entries);
            Rebuild();
            return;
        }
    }
    owners_.push_back(OwnerRules{owner, std::move(package_id), std::move(entries)});
    Rebuild();
}

void Registry::RemoveOwner(uint64_t owner) {
    const auto removed = std::erase_if(owners_, [&](const OwnerRules& item) { return item.owner == owner; });
    if (removed) Rebuild();
}

size_t Registry::RuleCount() const noexcept {
    return compiled_ ? compiled_->by_ext.size() : 0;
}

void Registry::SetChangedCallback(std::function<void()> changed) {
    changed_ = std::move(changed);
}

void Registry::Rebuild() {
    auto table = std::make_shared<ui::ext_capsule::Table>();
    // First registered package wins on extension conflicts.
    for (const auto& owner : owners_) {
        for (const auto& entry : owner.entries) {
            table->by_ext.try_emplace(entry.ext, entry.rule);
        }
    }
    compiled_ = table->by_ext.empty() ? nullptr : std::move(table);
    ++revision_;
    if (publish_to_ui_) ui::ext_capsule::Store::Instance().Replace(compiled_);
    if (changed_) {
        try { changed_(); } catch (...) {}
    }
}

}  // namespace pulse::extensions::extcapsules
