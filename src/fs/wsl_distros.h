// wsl_distros.h - registered WSL distributions without touching the WSL share.
#pragma once

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace pulse::fs {

struct WslDistribution {
    std::wstring name;
    std::wstring path;
};

inline bool IsValidWslDistributionName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L"..") return false;
    for (const wchar_t c : name) {
        if (iswcntrl(c) || c == L'\\' || c == L'/') return false;
    }
    return true;
}

inline bool SameWslDistributionName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline std::wstring WslDistributionPath(const std::wstring& name) {
    return L"\\\\?\\UNC\\wsl.localhost\\" + name;
}

inline std::vector<WslDistribution> EnumerateRegisteredWslDistributions() {
    constexpr wchar_t kLxssKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLxssKey, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return {};

    DWORD subkey_count = 0;
    DWORD max_subkey_length = 0;
    if (RegQueryInfoKeyW(root, nullptr, nullptr, nullptr, &subkey_count, &max_subkey_length,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        RegCloseKey(root);
        return {};
    }

    std::vector<WslDistribution> result;
    std::vector<wchar_t> subkey(max_subkey_length + 1);
    for (DWORD index = 0; index < subkey_count; ++index) {
        DWORD subkey_length = max_subkey_length + 1;
        if (RegEnumKeyExW(root, index, subkey.data(), &subkey_length, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            continue;

        HKEY distribution = nullptr;
        if (RegOpenKeyExW(root, subkey.data(), 0, KEY_QUERY_VALUE, &distribution) != ERROR_SUCCESS)
            continue;

        DWORD bytes = 0;
        constexpr DWORD kDistributionNameFlags =
            RRF_NOEXPAND | RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
        const LSTATUS query = RegGetValueW(distribution, nullptr, L"DistributionName",
                                           kDistributionNameFlags, nullptr, nullptr, &bytes);
        if (query != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
            RegCloseKey(distribution);
            continue;
        }
        std::wstring name(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(distribution, nullptr, L"DistributionName",
                         kDistributionNameFlags, nullptr, name.data(), &bytes) != ERROR_SUCCESS) {
            RegCloseKey(distribution);
            continue;
        }
        RegCloseKey(distribution);
        if (bytes % sizeof(wchar_t) != 0) continue;
        name.resize(bytes / sizeof(wchar_t));
        if (!name.empty() && name.back() == L'\0') name.pop_back();
        if (!IsValidWslDistributionName(name)) continue;
        if (std::any_of(result.begin(), result.end(), [&](const WslDistribution& existing) {
                return SameWslDistributionName(existing.name, name);
            }))
            continue;
        result.push_back({name, WslDistributionPath(name)});
    }
    RegCloseKey(root);

    std::stable_sort(result.begin(), result.end(), [](const WslDistribution& a, const WslDistribution& b) {
        const int by_name = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
        if (by_name != CSTR_EQUAL) return by_name == CSTR_LESS_THAN;
        return CompareStringOrdinal(a.path.c_str(), -1, b.path.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return result;
}

} // namespace pulse::fs
