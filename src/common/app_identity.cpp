#include "app_identity.h"
#include "string_ids.h"
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <shellapi.h>

namespace pulse::app_identity {
namespace {
HRESULT SetString(IPropertyStore* properties, REFPROPERTYKEY key, const wchar_t* text) {
    PROPVARIANT value{};
    HRESULT result = InitPropVariantFromString(text, &value);
    if (SUCCEEDED(result)) result = properties->SetValue(key, value);
    PropVariantClear(&value);
    return result;
}
}
HRESULT SetProcess() noexcept { return SetCurrentProcessExplicitAppUserModelID(kId); }
HRESULT SetShortcut(IPropertyStore* properties) noexcept {
    if (!properties) return E_POINTER;
    const HRESULT result = SetString(properties, PKEY_AppUserModel_ID, kId);
    return FAILED(result) ? result : properties->Commit();
}
HRESULT SetWindow(HWND window, const std::wstring& stable_entry) noexcept {
    if (!window || stable_entry.empty() || stable_entry.find(L'"') != std::wstring::npos) return E_INVALIDARG;
    try {
        const auto command = L"\"" + stable_entry + L"\" --activate";
        const auto title = L"@" + stable_entry + L",-" + std::to_wstring(IDS_INTEGRATION_HANDLER_PULSE);
        const auto icon = stable_entry + L",0";
        IPropertyStore* properties = nullptr;
        HRESULT result = SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties));
        if (FAILED(result)) return result;
        result = SetString(properties, PKEY_AppUserModel_RelaunchCommand, command.c_str());
        if (SUCCEEDED(result)) result = SetString(properties, PKEY_AppUserModel_RelaunchDisplayNameResource, title.c_str());
        if (SUCCEEDED(result)) result = SetString(properties, PKEY_AppUserModel_RelaunchIconResource, icon.c_str());
        // ID last: setting it notifies Explorer to read the complete relaunch data.
        if (SUCCEEDED(result)) result = SetString(properties, PKEY_AppUserModel_ID, kId);
        if (SUCCEEDED(result)) result = properties->Commit();
        properties->Release();
        return result;
    } catch (...) { return E_OUTOFMEMORY; }
}
void ClearWindow(HWND window) noexcept {
    IPropertyStore* properties = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties)))) return;
    const PROPVARIANT empty{};
    for (const auto& key : {PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchCommand,
            PKEY_AppUserModel_RelaunchDisplayNameResource, PKEY_AppUserModel_RelaunchIconResource})
        properties->SetValue(key, empty);
    properties->Commit();
    properties->Release();
}
}
