#pragma once

#include "../../ui/ext_capsule.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::extensions::extcapsules {

struct Entry {
    std::wstring ext;  // lowercase ASCII without leading dot
    ui::ext_capsule::Rule rule;
};

struct OwnerRules {
    uint64_t owner = 0;
    std::string package_id;
    std::vector<Entry> entries;
};

// Process-wide registry of plugin extension capsule rules (pulse.extcapsules).
// UI thread only. Publishes a flattened O(1) lookup table into ui::ext_capsule::Store
// (when attached to the global UI store) so the Direct2D renderer reads without
// depending on app/extensions.
class Registry {
public:
    explicit Registry(bool publish_to_ui = true);
    static Registry& Instance();

    void SetRules(uint64_t owner, std::string package_id, std::vector<Entry> entries);
    void RemoveOwner(uint64_t owner);
    size_t OwnerCount() const noexcept { return owners_.size(); }
    size_t RuleCount() const noexcept;
    std::shared_ptr<const ui::ext_capsule::Table> Compiled() const noexcept { return compiled_; }

    void SetChangedCallback(std::function<void()> changed);
    uint64_t Revision() const noexcept { return revision_; }

private:
    void Rebuild();

    bool publish_to_ui_ = true;
    std::vector<OwnerRules> owners_;
    std::shared_ptr<const ui::ext_capsule::Table> compiled_;
    uint64_t revision_ = 0;
    std::function<void()> changed_;
};

}  // namespace pulse::extensions::extcapsules
