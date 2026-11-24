#pragma once

#include "app_model.h"

namespace pulse::app {

inline const std::wstring& AddressSearchHandoffPath(const Tab& tab) noexcept {
    // A cleared content view keeps its query URI in memory so typing the same
    // query can run it again. A fresh window or restored tab must not replay it.
    if (tab.search_content_empty && tab.search_input_content &&
        tab.search_input_path == tab.current_path && tab.search_input_text.empty() &&
        tab.current_path.starts_with(L"pulse:search:")) {
        return tab.search_origin_valid ? tab.search_origin_path : tab.search_input_root;
    }
    return tab.current_path;
}

} // namespace pulse::app
