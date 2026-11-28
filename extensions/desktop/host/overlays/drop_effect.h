// drop_effect.h - 弹簧文件夹: what dropping files into a folder does (pure; shared by the Dock's drop
// zones and its stacks). Ctrl copies, Shift moves, otherwise a move on the same drive and a copy across
// drives, as in Explorer. DROPEFFECT_NONE when it cannot go there (no folder, every file already in
// it, or a folder into itself).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleidl.h>

#include <cwctype>
#include <string>
#include <vector>

namespace pulse::desktop::overlays {

namespace drop_detail {
inline std::wstring DropKey(std::wstring s) {
    for (auto& ch : s) ch = (ch == L'/') ? L'\\' : static_cast<wchar_t>(std::towlower(ch));
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}
inline std::wstring VolumeOf(const std::wstring& key) {
    if (key.size() >= 2 && key[1] == L':') return key.substr(0, 2);
    if (key.rfind(L"\\\\", 0) == 0) {   // \\server\share
        const size_t a = key.find(L'\\', 2);
        const size_t b = a == std::wstring::npos ? a : key.find(L'\\', a + 1);
        return key.substr(0, b);
    }
    return {};
}
} // namespace drop_detail

inline DWORD DockStackDropEffect(const std::vector<std::wstring>& paths, const std::wstring& dest_folder,
                          DWORD key_state) noexcept {
    if (dest_folder.empty() || paths.empty()) return DROPEFFECT_NONE;
    const std::wstring dest = drop_detail::DropKey(dest_folder);
    bool any_new = false, same_volume = true;
    const std::wstring dest_vol = drop_detail::VolumeOf(dest);
    for (const auto& p : paths) {
        const std::wstring src = drop_detail::DropKey(p);
        if (src.empty()) continue;
        if (dest == src || (dest.size() > src.size() && dest.compare(0, src.size(), src) == 0 && dest[src.size()] == L'\\'))
            return DROPEFFECT_NONE;   // a folder into itself
        const size_t slash = src.find_last_of(L'\\');
        if (slash == std::wstring::npos || src.substr(0, slash) != dest) any_new = true;
        if (drop_detail::VolumeOf(src) != dest_vol || dest_vol.empty()) same_volume = false;
    }
    if (!any_new) return DROPEFFECT_NONE;
    if (key_state & MK_CONTROL) return DROPEFFECT_COPY;
    if (key_state & MK_SHIFT) return DROPEFFECT_MOVE;
    return same_volume ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
}


} // namespace pulse::desktop::overlays
