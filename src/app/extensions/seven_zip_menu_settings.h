#pragma once

#include "../../common/json_value.h"
#include "../../../extensions/7zip/command_preferences.h"
#include <memory>
#include <mutex>
#include <optional>

namespace pulse::extensions::seven_zip_menu {

struct Cache {
    std::mutex mutex;
    bool reading = false;
    uint64_t version = 0;
    std::shared_ptr<const std::string> encoded;
    std::optional<bool> preview_on_open_edit;
};

inline Cache& PreferencesCache() {
    static Cache cache;
    return cache;
}

inline std::shared_ptr<const std::string> Snapshot() {
    auto& cache = PreferencesCache();
    std::lock_guard lock(cache.mutex);
    return cache.encoded;
}

inline bool BeginRead() {
    auto& cache = PreferencesCache();
    std::lock_guard lock(cache.mutex);
    if (cache.reading || cache.encoded) return false;
    cache.reading = true;
    return true;
}

inline void SetPreviewOnOpen(bool preview) {
    auto& cache = PreferencesCache();
    std::lock_guard lock(cache.mutex);
    cache.preview_on_open_edit = preview;
}

inline bool PreviewOnOpen() {
    auto& cache = PreferencesCache();
    std::lock_guard lock(cache.mutex);
    if (cache.preview_on_open_edit) return *cache.preview_on_open_edit;
    // A cold or unreadable profile must not turn a preview preference into
    // extraction. A successfully read profile with no setting uses the default.
    if (!cache.encoded) return true;
    if (cache.encoded->empty()) return false;
    const auto prefs = seven_zip::ParseCommandPreferences(*cache.encoded);
    return !prefs || prefs->preview_on_open;
}

inline void Publish(const json::Value& result, bool read_completed = false) {
    auto& cache = PreferencesCache();
    std::lock_guard lock(cache.mutex);
    if (read_completed) cache.reading = false;
    const auto* data = result.find("data");
    const auto* version = data ? data->find("version") : nullptr;
    const auto* values = data ? data->find("values") : nullptr;
    if (!version || !values) return;
    const auto revision = static_cast<uint64_t>(version->as_number());
    if (cache.encoded && revision < cache.version) return;
    const auto* value = values->find(seven_zip::kPreferencesKey);
    cache.encoded = std::make_shared<const std::string>(value ? value->as_string() : std::string{});
    cache.version = revision;
}

} // namespace pulse::extensions::seven_zip_menu
