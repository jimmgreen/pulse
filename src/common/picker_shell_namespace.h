#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <cstdint>

namespace pulse::picker_shell {

inline bool IsParsingName(std::wstring_view name) noexcept {
    if (name.starts_with(L"::{")) return true;
    const auto colon = name.find(L':');
    if (colon == name.npos || colon < 2 || name.starts_with(L"pulse:")) return false;
    for (size_t i = 0; i < colon; ++i) {
        const wchar_t c = name[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (i && ((c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.')))) return false;
    }
    return true;
}

inline bool Enabled(uint32_t options) noexcept {
    return (options & (FOS_ALLNONSTORAGEITEMS | FOS_SUPPORTSTREAMABLEITEMS)) != 0;
}

inline HRESULT ValidateOptions(uint32_t options) noexcept {
    return (options & FOS_ALLNONSTORAGEITEMS) && (options & FOS_FORCEFILESYSTEM) ? E_INVALIDARG : S_OK;
}

inline bool IsStream(SFGAOF attributes, uint32_t options) noexcept {
    return (options & FOS_SUPPORTSTREAMABLEITEMS) && (attributes & SFGAO_STREAM);
}

inline bool IsSelectable(SFGAOF attributes, bool pick_folder, uint32_t options) noexcept {
    if (FAILED(ValidateOptions(options))) return false;
    if ((options & FOS_FORCEFILESYSTEM) && !(attributes & SFGAO_FILESYSTEM)) return false;
    const bool folder = (attributes & SFGAO_FOLDER) != 0;
    if (pick_folder != folder && !(folder && !pick_folder && IsStream(attributes, options))) return false;
    return (attributes & SFGAO_FILESYSTEM) || (options & FOS_ALLNONSTORAGEITEMS) || IsStream(attributes, options);
}

inline HRESULT Name(IShellItem* item, SIGDN kind, std::wstring& name) {
    name.clear();
    if (!item) return E_POINTER;
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(kind, &raw);
    struct Release { PWSTR value; ~Release() { CoTaskMemFree(value); } } release{raw};
    if (SUCCEEDED(hr) && raw) name.assign(raw);
    return FAILED(hr) ? hr : name.empty() ? E_UNEXPECTED : S_OK;
}

// Navigation history already retains exact parent names. This is the fallback
// for the Up button; a namespace root returns to This PC without doing COM I/O.
inline std::wstring Parent(std::wstring_view name) {
    const auto slash = name.find_last_of(L'\\');
    return slash == name.npos ? std::wstring{} : std::wstring(name.substr(0, slash));
}

class Apartment {
public:
    Apartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~Apartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    HRESULT Result() const noexcept { return result_ == RPC_E_CHANGED_MODE ? S_OK : result_; }
private:
    HRESULT result_;
};

inline DWORD Error(HRESULT hr) noexcept {
    if (SUCCEEDED(hr)) return ERROR_SUCCESS;
    if (hr == STG_E_SHAREVIOLATION) return ERROR_SHARING_VIOLATION;
    if (hr == STG_E_ACCESSDENIED) return ERROR_ACCESS_DENIED;
    if (hr == STG_E_FILENOTFOUND) return ERROR_FILE_NOT_FOUND;
    if (hr == STG_E_PATHNOTFOUND) return ERROR_PATH_NOT_FOUND;
    if (hr == STG_E_INVALIDNAME) return ERROR_INVALID_NAME;
    if (hr == STG_E_LOCKVIOLATION) return ERROR_LOCK_VIOLATION;
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_NOT_SUPPORTED;
}

// Worker-only access validation. Request a read stream without reading data,
// creating a file, or retaining a provider object past this call.
inline HRESULT ValidateStream(IShellItem* item) noexcept {
    if (!item) return E_POINTER;
    Microsoft::WRL::ComPtr<IBindCtx> context;
    HRESULT hr = CreateBindCtx(0, &context);
    if (FAILED(hr)) return hr;
    BIND_OPTS options{}; options.cbStruct = sizeof(options);
    options.grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
    hr = context->SetBindOptions(&options);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IStream> stream;
    hr = item->BindToHandler(context.Get(), BHID_Stream, IID_PPV_ARGS(&stream));
    return FAILED(hr) ? hr : stream ? S_OK : E_UNEXPECTED;
}

} // namespace pulse::picker_shell
