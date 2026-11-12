#pragma once
#include "file_dialog_inprocess_session.h"

namespace pulse::shell_bridge {
inline uint32_t DynamicTextKind(Setting setting) noexcept {
    switch (setting) {
    case Setting::Title: return PULSE_PICKER_UI_V2_TEXT_TITLE;
    case Setting::OkButtonLabel: return PULSE_PICKER_UI_V2_TEXT_OK_LABEL;
    case Setting::FileNameLabel: return PULSE_PICKER_UI_V2_TEXT_FILENAME_LABEL;
    case Setting::FileName: return PULSE_PICKER_UI_V2_TEXT_FILENAME;
    case Setting::DefaultExtension: return PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION;
    default: return 0;
    }
}
inline HRESULT RestoreControlItems(IFileDialogCustomize* native, const FileDialogCustomControl& before,
    const FileDialogCustomControl& after) {
    // RemoveAllControlItems is E_NOTIMPL on native dialogs, including live Show.
    // Rebuild by ID so a failed UI update also restores ordering and selection.
    for (const auto& item : after.items) {
        const HRESULT hr = native->RemoveControlItem(before.id, item.id);
        if (hr != S_OK) return hr;
    }
    for (const auto& item : before.items) {
        HRESULT hr = native->AddControlItem(before.id, item.id, item.label.c_str());
        if (hr != S_OK) return hr;
        hr = native->SetControlItemState(before.id, item.id, static_cast<CDCONTROLSTATEF>(item.state));
        if (hr != S_OK) return hr;
    }
    if (before.has_selected) {
        const HRESULT hr = native->SetSelectedControlItem(before.id, before.selected_item_id);
        if (hr != S_OK) return hr;
    }
    DWORD selected = 0;
    const HRESULT queried = native->GetSelectedControlItem(before.id, &selected);
    return (queried == S_OK) == before.has_selected && (!before.has_selected || selected == before.selected_item_id) ? S_OK : E_UNEXPECTED;
}
}
