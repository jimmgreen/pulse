#pragma once
#include "picker_ui_api_v2.h"
#include <string>

namespace pulse::picker_inprocess {
inline bool NormalizeDynamicText(uint32_t kind, std::wstring& text, bool save, bool native_save_defaults) {
    if (kind < PULSE_PICKER_UI_V2_TEXT_TITLE || kind > PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION) return false;
    if (kind <= PULSE_PICKER_UI_V2_TEXT_FILENAME_LABEL)
        return text.size() <= 1024 && text.find_first_of(L"\r\n\t") == text.npos;
    if (kind != PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION) return true;
    if (text.size() > 255) return false;
    if (save && native_save_defaults && text.starts_with(L"*.")) {
        text.erase(0, 2);
        if (text.empty()) return false;
    }
    if (text.find_first_of(save ? L".\\/:*?\"<>|; \t\r\n" : L"\\/:\"<>\t\r\n") != text.npos) return false;
    if (!save) {
        for (const auto unit : text) if (unit < 32) return false;
        if (!text.empty() && (text.back() == L'.' || text.back() == L' ')) return false;
    }
    return true;
}
}
