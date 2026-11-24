#pragma once

#include <windows.h>
#include <shlobj.h>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::known_folders {

inline std::wstring CurrentPath(REFKNOWNFOLDERID id, DWORD flags = KF_FLAG_DONT_VERIFY) {
    PWSTR raw = nullptr;
    const HRESULT status = SHGetKnownFolderPath(id, flags, nullptr, &raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
    return SUCCEEDED(status) && path ? std::wstring(path.get()) : std::wstring{};
}

inline bool IsSettingsArea(const wchar_t* area) noexcept {
    if (!area) return false;
    std::wstring_view key(area);
    if (const auto slash = key.find_last_of(L"\\/"); slash != key.npos) key.remove_prefix(slash + 1);
    const auto equals = [key](std::wstring_view name) {
        return key.size() == name.size() &&
            CompareStringOrdinal(key.data(), static_cast<int>(key.size()),
                name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    };
    return equals(L"User Shell Folders") || equals(L"Shell Folders") || equals(L"Folder Redirection");
}

} // namespace pulse::known_folders
