#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app {
// Stores only display fields; the source Settings and credentials stay local to refresh.
struct AssistantSettingsFields {
    uint64_t at = 0;
    unsigned epoch = 0;
    std::string voice_mic;
    std::vector<std::pair<std::string, std::wstring>> setup_urls;

    bool NeedsRefresh(uint64_t now, unsigned current_epoch) const {
        return !at || now - at >= 2000 || epoch != current_epoch;
    }

    template<class Settings, class Providers, class Url>
    void Assign(const Settings& settings, const Providers& providers, Url url) {
        voice_mic = settings.assistant.voice.mic;
        setup_urls.clear();
        for (const auto& provider : providers)
            setup_urls.emplace_back(provider.id, url(settings, provider));
    }

    std::wstring SetupUrl(const std::string& id) const {
        for (const auto& entry : setup_urls) if (entry.first == id) return entry.second;
        return {};
    }
};
} // namespace pulse::app
