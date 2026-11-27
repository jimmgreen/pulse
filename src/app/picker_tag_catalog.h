#pragma once
#include "entry_group.h"
#include "../common/picker_tag_catalog_shared.h"
#include <cwctype>

namespace pulse::app {
inline std::shared_ptr<const TagCatalogSnapshot> ReadPickerTagCatalog(const std::wstring& name = {}) {
    auto shared = name.empty() ? picker_tags::Read() : picker_tags::Read(name);
    if (!shared.available) return {};
    auto catalog = std::make_shared<TagCatalogSnapshot>();
    catalog->revision = shared.revision;
    catalog->paths = std::move(shared.paths);
    for (auto& [path, rank] : catalog->paths) {
        (void)rank;
        for (auto& ch : path) ch = static_cast<wchar_t>(std::towlower(ch));
    }
    catalog->tags.reserve(shared.tags.size());
    for (auto& tag : shared.tags) catalog->tags.push_back({std::move(tag.name), tag.rgb});
    return catalog;
}

// UI-owned immutable catalog; no file or ADS reads, and no per-tick re-encoding.
inline void PublishPickerTagCatalog() {
    static std::shared_ptr<const TagCatalogSnapshot> previous;
    static picker_tags::Snapshot shared;
    const auto catalog = CurrentTagCatalog();
    if (catalog != previous) {
        previous = catalog;
        shared = {};
        if (catalog) {
            shared.available = true;
            shared.revision = catalog->revision;
            shared.paths = catalog->paths;
            shared.tags.reserve(catalog->tags.size());
            for (const auto& tag : catalog->tags) shared.tags.push_back({tag.name, tag.rgb});
        }
    }
    picker_tags::Publish(shared);
}
}
