#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse::ops::archive_open {
inline bool Supported(std::wstring_view path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == path.npos) return false;
    auto extension = std::wstring(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    constexpr std::wstring_view extensions[]{L".7z", L".zip", L".rar", L".tar", L".gz", L".tgz",
        L".xz", L".txz", L".bz2", L".tbz2", L".tbz", L".lzma", L".z", L".cab", L".arj",
        L".lzh", L".lha", L".cpio", L".zipx"};
    return std::find(std::begin(extensions), std::end(extensions), extension) != std::end(extensions);
}
inline bool MissingAssociation(HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) ||
        result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
inline bool ShouldExtract(HRESULT result, std::wstring_view progid) noexcept {
    // Preserve custom/delegated defaults. Windows' inherited ZIP-folder viewer
    // is not a separately registered archive application.
    return MissingAssociation(result) || (SUCCEEDED(result) &&
        (CompareStringOrdinal(progid.data(), static_cast<int>(progid.size()), L"CompressedFolder", -1, TRUE) == CSTR_EQUAL ||
         CompareStringOrdinal(progid.data(), static_cast<int>(progid.size()), L"ArchiveFolder", -1, TRUE) == CSTR_EQUAL));
}
inline HRESULT CurrentDefault(const wchar_t* extension, std::wstring& progid) {
    progid.clear();
    Microsoft::WRL::ComPtr<IApplicationAssociationRegistration> registration;
    auto result = CoCreateInstance(__uuidof(ApplicationAssociationRegistration), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(registration.GetAddressOf()));
    if (FAILED(result)) return result;
    PWSTR value = nullptr;
    result = registration->QueryCurrentDefault(extension, AT_FILEEXTENSION, AL_EFFECTIVE, &value);
    try { if (SUCCEEDED(result) && value) progid = value; }
    catch (...) { result = E_OUTOFMEMORY; }
    CoTaskMemFree(value);
    return result;
}
// Called only by the shell-open STA, never while handling a UI click.
inline bool NeedsFallback(const std::wstring& path) {
    if (!Supported(path)) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const auto extension = path.substr(path.find_last_of(L'.'));
    std::wstring progid;
    const auto result = CurrentDefault(extension.c_str(), progid);
    return ShouldExtract(result, progid);
}
}
