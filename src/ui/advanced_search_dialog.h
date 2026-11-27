// advanced_search_dialog.h — Structured search form with filename regex matching.
#pragma once
#include "../app/search_query.h"
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <string>

namespace pulse::ui {

struct AdvancedSearchDialogResult {
    bool accepted = false;
    std::wstring query;
};

// Posted to the dialog with the live match count in WPARAM.
inline constexpr UINT kAdvancedSearchCountMessage = WM_APP + 0x2C1;
// Asks the host to count filename matches for `query` and post
// kAdvancedSearchCountMessage back to `dialog`. Called debounced while editing.
using AdvancedSearchCountRequest = std::function<void(HWND dialog, const std::wstring& query)>;

AdvancedSearchDialogResult ShowAdvancedSearchDialog(HWND owner,
                                                    app::AdvancedSearchSpec spec,
                                                    bool dark,
                                                    D2D1_COLOR_F accent,
                                                    AdvancedSearchCountRequest count = {},
                                                    bool selection_outline = false);

} // namespace pulse::ui
