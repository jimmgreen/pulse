// known_extensions.h - 文件扩展名 off (AppPrefs::show_file_extensions): as
// with File Explorer's "hide extensions for known file types", a file whose
// type is registered is drawn without its extension. Only the drawn name
// changes: the item's name, its icon, sorting and rename keep the extension
// (rename selects the part before it, as always).
#pragma once
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pulse::ui {

namespace known_extensions_detail {
inline std::atomic<bool>& Hide() noexcept {
    static std::atomic<bool> hide{false};
    return hide;
}
inline std::atomic<uint64_t>& Revision() noexcept {
    static std::atomic<uint64_t> revision{1};
    return revision;
}
inline bool HasValue(HKEY key, const wchar_t* name) noexcept {
    return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
// Registered: HKCR\.ext names a file type or lists some under
// OpenWithProgids, and neither it nor the type asks for AlwaysShowExt.
inline bool Registered(const std::wstring& ext) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, ext.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t type_name[256]{};
    DWORD bytes = sizeof(type_name) - sizeof(wchar_t);
    DWORD kind = 0;
    const bool named = RegQueryValueExW(key, nullptr, nullptr, &kind, reinterpret_cast<BYTE*>(type_name), &bytes) ==
                           ERROR_SUCCESS &&
                       (kind == REG_SZ || kind == REG_EXPAND_SZ) && type_name[0];
    bool always = HasValue(key, L"AlwaysShowExt");
    HKEY listed_key = nullptr;
    const bool listed = RegOpenKeyExW(key, L"OpenWithProgids", 0, KEY_READ, &listed_key) == ERROR_SUCCESS;
    if (listed_key) RegCloseKey(listed_key);
    RegCloseKey(key);
    if (named && !always) {
        HKEY type_key = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, type_name, 0, KEY_READ, &type_key) == ERROR_SUCCESS) {
            always = HasValue(type_key, L"AlwaysShowExt");
            RegCloseKey(type_key);
        }
    }
    return (named || listed) && !always;
}
}  // namespace known_extensions_detail

inline bool HideKnownExtensions() noexcept { return known_extensions_detail::Hide().load(std::memory_order_relaxed); }
// Rows built before a change rebuild when this moves.
inline uint64_t KnownExtensionsRevision(int show_extensions = -1) noexcept {
    return show_extensions < 0 ? known_extensions_detail::Revision().load(std::memory_order_relaxed) << 2
                              : (show_extensions ? 1 : 2);
}
inline void SetHideKnownExtensions(bool hide) noexcept {
    if (known_extensions_detail::Hide().exchange(hide, std::memory_order_relaxed) != hide)
        known_extensions_detail::Revision().fetch_add(1, std::memory_order_relaxed);
}

// How many trailing characters of `name` are not drawn: the extension with
// its dot while hiding is on and the type is registered, else 0. Folders
// keep their whole name, as does a name that is only an extension
// (".gitignore"). The registry answer is kept per extension.
inline size_t HiddenExtensionLength(std::wstring_view name, bool is_dir, bool hide = HideKnownExtensions()) {
    if (is_dir || !hide) return 0;
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 == name.size() || name.size() - dot > 64) return 0;
    std::wstring ext(name.substr(dot));
    CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
    static std::mutex mu;
    static std::unordered_map<std::wstring, bool> known;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (const auto found = known.find(ext); found != known.end()) return found->second ? name.size() - dot : 0;
    }
    const bool registered = known_extensions_detail::Registered(ext);
    std::lock_guard<std::mutex> lock(mu);
    if (known.size() >= 4096) known.clear();
    known.emplace(std::move(ext), registered);
    return registered ? name.size() - dot : 0;
}

}  // namespace pulse::ui
