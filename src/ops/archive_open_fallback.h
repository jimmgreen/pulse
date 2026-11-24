#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

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
// The 7-Zip extension's own file type (接管压缩包, extensions/7zip/takeover_record.h) whose
// double-click extracts: run that in place instead of starting Pulse again through the shell.
inline bool PulseExtractType(std::wstring_view progid) {
    constexpr std::wstring_view prefix = L"Pulse.Archive.";
    if (progid.size() <= prefix.size() || CompareStringOrdinal(progid.data(), static_cast<int>(prefix.size()),
                                                                prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
        return false;
    const std::wstring key = L"Software\\Classes\\" + std::wstring(progid) + L"\\shell\\open\\command";
    wchar_t command[2048]{};
    DWORD bytes = sizeof(command) - sizeof(wchar_t);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, command, &bytes) == ERROR_SUCCESS &&
        std::wstring_view(command).find(L"\"pulse:extract:") != std::wstring_view::npos;
}
// Called only by the shell-open STA, never while handling a UI click.
inline bool NeedsFallback(const std::wstring& path) {
    if (!Supported(path)) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const auto extension = path.substr(path.find_last_of(L'.'));
    std::wstring progid;
    const auto result = CurrentDefault(extension.c_str(), progid);
    return ShouldExtract(result, progid) || (SUCCEEDED(result) && PulseExtractType(progid));
}

inline bool PulseExtensionType(std::wstring_view progid) {
    constexpr std::wstring_view prefix = L"Pulse.Ext.";
    if (progid.size() <= prefix.size() || CompareStringOrdinal(progid.data(), static_cast<int>(prefix.size()),
                                                                prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
        return false;
    const std::wstring key = L"Software\\Classes\\" + std::wstring(progid) + L"\\shell\\open\\command";
    wchar_t command[2048]{};
    DWORD bytes = sizeof(command) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, command, &bytes) != ERROR_SUCCESS)
        return false;
    const std::wstring_view cmd(command);
    return cmd.find(L"--ext-open") != std::wstring_view::npos || cmd.find(L"--ext-preview") != std::wstring_view::npos;
}

inline bool NeedsCustomFallback(const std::wstring& path, const std::vector<std::wstring>& extra_extensions) {
    if (extra_extensions.empty() || path.empty()) return false;
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring lower_name(slash == std::wstring::npos ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1));
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), towlower);
    std::wstring matched_ext;
    for (const auto& ext : extra_extensions) {
        if (ext.size() > matched_ext.size() && !ext.empty() && ext.front() == L'.' &&
            lower_name.size() >= ext.size() && lower_name.ends_with(ext)) {
            matched_ext = ext;
        }
    }
    if (matched_ext.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const size_t dot = lower_name.find_last_of(L'.');
    const std::wstring single_ext = dot == std::wstring::npos ? matched_ext : lower_name.substr(dot);
    std::wstring progid;
    const auto result = CurrentDefault(single_ext.c_str(), progid);
    return MissingAssociation(result) || (SUCCEEDED(result) && PulseExtensionType(progid));
}
}
