#pragma once
#include <windows.h>
#include <d2d1.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

struct QuickPickDialogItem {
    std::string id;
    std::wstring title;
    std::wstring detail;
    std::wstring glyph;
    std::wstring shortcut;
    bool selected = false;
    bool disabled = false;
};

struct QuickPickDialogSpec {
    std::wstring title;
    std::wstring placeholder;
    std::wstring initial_query;
    std::vector<QuickPickDialogItem> items;
    HANDLE cancel_event = nullptr;
};

enum class QuickPickDialogStatus : uint32_t {
    Accepted = 0,
    Cancelled = 1,
    Failed = 2,
};

struct QuickPickDialogOutcome {
    QuickPickDialogStatus status = QuickPickDialogStatus::Failed;
    std::string selected_id;
    DWORD error = ERROR_SUCCESS;
};

QuickPickDialogOutcome ShowQuickPickDialog(HWND owner, const QuickPickDialogSpec& spec,
                                           bool dark, D2D1_COLOR_F accent);

} // namespace pulse::ui
