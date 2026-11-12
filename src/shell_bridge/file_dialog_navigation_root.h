#pragma once
#include <shobjidl.h>
#include <string>

namespace pulse::shell_bridge {
inline HRESULT CaptureNavigationRoot(IShellItem* item, std::wstring& path) {
    if (!item) return E_INVALIDARG;
    SFGAOF flags = 0;
    HRESULT hr = item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &flags);
    if (FAILED(hr)) return hr;
    if ((flags & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) != (SFGAO_FILESYSTEM | SFGAO_FOLDER)) return E_NOTIMPL;
    PWSTR value = nullptr; hr = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
    if (hr == S_OK && value) {
        try { path = value; } catch (...) { CoTaskMemFree(value); return E_OUTOFMEMORY; }
    }
    CoTaskMemFree(value);
    return FAILED(hr) ? hr : path.empty() ? E_NOTIMPL : S_OK;
}
}
