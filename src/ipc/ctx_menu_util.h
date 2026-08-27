// ctx_menu_util.h — Shared context-menu verb filtering / text helpers.
//
// Used by pulse_shell.exe (filters QueryContextMenu output before it goes on
// the wire) and by the pulse.exe self-test (the HMENU walk itself is host-only,
// but the filtering rules are pure and asserted here). Header-only on purpose.
#pragma once
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ipc {

inline std::wstring ToLowerVerb(std::wstring_view v) {
    std::wstring out(v);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

// Canonical verbs Pulse already provides natively (or explicitly hides).
// These are dropped from the Explorer section so the merged menu has no
// duplicates. `background` = folder background menu (paste/new-area rules).
inline bool IsBuiltinContextVerb(std::wstring_view verb, bool background) {
    const std::wstring v = ToLowerVerb(verb);
    if (v.empty()) return false;
    static constexpr std::wstring_view kCommon[] = {
        L"open", L"opennewprocess", L"opennewwindow", L"opennewtab",
        L"explore", L"openas", L"cut", L"copy", L"paste", L"pastelink",
        L"delete", L"rename", L"properties", L"copyaspath",
        L"pintohome", L"pintostartscreen", L"pintostart",
        L"windows.modernshare", L"windows.share",
    };
    for (auto k : kCommon)
        if (v == k) return true;
    if (background) {
        static constexpr std::wstring_view kBackground[] = {
            L"undo", L"redo", L"refresh", L"view", L"arrange",
            L"sortby", L"groupby", L"viewcustomwizard", L"wallpaper",
        };
        for (auto k : kBackground)
            if (v == k) return true;
    }
    return false;
}

// Submenu parents that are dropped entirely (their content is provided by
// Pulse from the registry instead, so flattening them would duplicate rows).
inline bool IsDroppedContextSubmenu(std::wstring_view verb) {
    const std::wstring v = ToLowerVerb(verb);
    return v == L"openas" || v == L"open with";
}

// Menu display text -> clean row text: strip '&' accelerator markers and any
// "\tCtrl+X" shortcut suffix.
inline std::wstring CleanMenuText(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\t') break;
        if (raw[i] == L'&') {
            // "&&" means a literal ampersand; a single '&' marks the mnemonic.
            if (i + 1 < raw.size() && raw[i + 1] == L'&') {
                out += L'&';
                ++i;
            }
            continue;
        }
        out += raw[i];
    }
    // CJK menu items often carry "(&O)" mnemonics; after dropping '&' this
    // leaves "(O)" which is Explorer-authentic, keep it. Trim edges only.
    while (!out.empty() && out.back() == L' ') out.pop_back();
    size_t start = 0;
    while (start < out.size() && out[start] == L' ') ++start;
    return out.substr(start);
}

// At most this many children per software-owned submenu flyout.
inline constexpr int kMaxSubmenuChildren = 16;

// Categories for the Explorer fusion-zone prefs (context_menu.json). Host still
// returns the full list; the UI process decides enabled from these + overrides.
enum class CtxMenuCategory {
    Software = 0,
    Share,
    Wallpaper,
    Rotate,
    Shortcut,
    OpenWith,
    SystemExtra,
    Print,
};

enum class CtxMenuGroup {
    Software = 0,
    OpenWith,
    Share,
    System,
    Print,
};

inline bool MenuTextContainsI(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    const std::wstring h = ToLowerVerb(hay);
    const std::wstring n = ToLowerVerb(needle);
    return h.find(n) != std::wstring::npos;
}

inline std::wstring_view CtxMenuCategoryId(CtxMenuCategory c) {
    switch (c) {
    case CtxMenuCategory::Share: return L"share";
    case CtxMenuCategory::Wallpaper: return L"wallpaper";
    case CtxMenuCategory::Rotate: return L"rotate";
    case CtxMenuCategory::Shortcut: return L"shortcut";
    case CtxMenuCategory::OpenWith: return L"open_with";
    case CtxMenuCategory::SystemExtra: return L"system_extra";
    case CtxMenuCategory::Print: return L"print";
    default: return L"software";
    }
}

inline CtxMenuCategory ParseCtxMenuCategory(std::wstring_view id) {
    if (id == L"share") return CtxMenuCategory::Share;
    if (id == L"wallpaper") return CtxMenuCategory::Wallpaper;
    if (id == L"rotate") return CtxMenuCategory::Rotate;
    if (id == L"shortcut") return CtxMenuCategory::Shortcut;
    if (id == L"open_with") return CtxMenuCategory::OpenWith;
    if (id == L"system_extra") return CtxMenuCategory::SystemExtra;
    if (id == L"print") return CtxMenuCategory::Print;
    return CtxMenuCategory::Software;
}

inline CtxMenuGroup GroupOf(CtxMenuCategory c) {
    switch (c) {
    case CtxMenuCategory::Share: return CtxMenuGroup::Share;
    case CtxMenuCategory::Wallpaper:
    case CtxMenuCategory::Rotate:
    case CtxMenuCategory::Shortcut:
    case CtxMenuCategory::SystemExtra: return CtxMenuGroup::System;
    case CtxMenuCategory::OpenWith: return CtxMenuGroup::OpenWith;
    case CtxMenuCategory::Print: return CtxMenuGroup::Print;
    default: return CtxMenuGroup::Software;
    }
}

// Catalog key: cleaned lowercase text, flyout vs top-level verb kept distinct.
inline std::wstring CatalogKey(std::wstring_view text, bool is_flyout) {
    std::wstring t = ToLowerVerb(CleanMenuText(text));
    return (is_flyout ? L"f:" : L"v:") + t;
}

inline bool IsOpenWithPickerText(std::wstring_view text) {
    const std::wstring t = ToLowerVerb(CleanMenuText(text));
    return t.find(L"打开方式") != std::wstring::npos ||
           t.find(L"choose default") != std::wstring::npos ||
           t.find(L"open with\u2026") != std::wstring::npos ||
           t.find(L"open with...") != std::wstring::npos;
}

inline bool IsOpenWithMruText(std::wstring_view text) {
    if (IsOpenWithPickerText(text)) return false;
    const std::wstring t = ToLowerVerb(CleanMenuText(text));
    if (t.size() >= 2 && t[0] == L'用' && t.find(L"打开") != std::wstring::npos)
        return true;
    if (t.find(L"open with ") == 0 || t.find(L"edit with ") == 0) return true;
    return false;
}

inline const wchar_t* CompressCatalogKey() { return L"v:compress-to"; }
inline const wchar_t* CompressCatalogText() { return L"压缩为…"; }
inline const wchar_t* CompressFlyoutText() { return L"压缩"; }

inline std::wstring HandlerCatalogKey(std::wstring_view clsid) {
    return L"h:" + ToLowerVerb(clsid);
}
inline bool IsHandlerCatalogKey(std::wstring_view key) {
    return key.size() > 2 && key[0] == L'h' && key[1] == L':';
}
inline std::wstring HandlerClsidFromKey(std::wstring_view key) {
    return IsHandlerCatalogKey(key) ? std::wstring(key.substr(2)) : std::wstring{};
}
inline bool IsDisabledHandler(std::wstring_view clsid,
                              const std::vector<std::wstring>& disabled) {
    const std::wstring c = ToLowerVerb(clsid);
    if (c.empty()) return false;
    for (const auto& d : disabled)
        if (ToLowerVerb(d) == c) return true;
    return false;
}

inline bool IsCompressVendorFlyout(std::wstring_view text) {
    return MenuTextContainsI(text, L"bandizip") ||
           MenuTextContainsI(text, L"7-zip") ||
           MenuTextContainsI(text, L"7zip") ||
           MenuTextContainsI(text, L"winrar") ||
           MenuTextContainsI(text, L"nanazip") ||
           MenuTextContainsI(text, L"peazip") ||
           MenuTextContainsI(text, L"压缩");
}

inline bool IsCompressTopLevel(std::wstring_view text) {
    const std::wstring t = ToLowerVerb(CleanMenuText(text));
    if (t.find(L"压缩为") != std::wstring::npos) return true;
    if (t.find(L"新建压缩包") != std::wstring::npos) return true;
    if (t.find(L"compress to") != std::wstring::npos) return true;
    if (t.find(L"add to archive") != std::wstring::npos) return true;
    if (t.find(L"add to \"") != std::wstring::npos) return true;
    if (t.find(L"添加到\"") != std::wstring::npos || t.find(L"添加到「") != std::wstring::npos)
        return true;
    return false;
}

inline CtxMenuCategory ClassifyExplorerItem(std::wstring_view verb,
                                            std::wstring_view text,
                                            bool is_flyout) {
    const std::wstring v = ToLowerVerb(verb);
    const std::wstring t = ToLowerVerb(CleanMenuText(text));

    if (v == L"print" || t == L"打印" || t == L"print" ||
        t.find(L"打印(") == 0 || t.find(L"print(") == 0)
        return CtxMenuCategory::Print;

    if (v == L"sendto" || v == L"send to" ||
        v.find(L"share") != std::wstring::npos ||
        t.find(L"发送到") != std::wstring::npos ||
        t.find(L"发送给") != std::wstring::npos ||
        t == L"分享" || t.find(L"share") != std::wstring::npos)
        return CtxMenuCategory::Share;
    if (is_flyout && (t.find(L"qq") != std::wstring::npos ||
                      t.find(L"内网通") != std::wstring::npos ||
                      t.find(L"泛泰") != std::wstring::npos ||
                      t.find(L"快传") != std::wstring::npos))
        return CtxMenuCategory::Share;

    if (v.find(L"wallpaper") != std::wstring::npos ||
        t.find(L"桌面背景") != std::wstring::npos ||
        t.find(L"壁纸") != std::wstring::npos ||
        t.find(L"wallpaper") != std::wstring::npos)
        return CtxMenuCategory::Wallpaper;

    if (v.find(L"rotate") != std::wstring::npos ||
        t.find(L"旋转") != std::wstring::npos ||
        t.find(L"rotate") != std::wstring::npos)
        return CtxMenuCategory::Rotate;

    if (v == L"link" || v == L"createlink" ||
        t.find(L"创建快捷方式") != std::wstring::npos ||
        t.find(L"create shortcut") != std::wstring::npos)
        return CtxMenuCategory::Shortcut;

    if (v == L"openas" || v == L"openwith" || v == L"__openwith" || v == L"open with" ||
        IsOpenWithPickerText(text) || IsOpenWithMruText(text))
        return CtxMenuCategory::OpenWith;

    if (t.find(L"以前的版本") != std::wstring::npos ||
        t.find(L"previous version") != std::wstring::npos ||
        t.find(L"始终脱机") != std::wstring::npos ||
        t.find(L"脱机可用") != std::wstring::npos ||
        t.find(L"always available offline") != std::wstring::npos ||
        t.find(L"数字签名") != std::wstring::npos ||
        t.find(L"digital signature") != std::wstring::npos ||
        t.find(L"兼容性疑难") != std::wstring::npos ||
        t.find(L"troubleshoot compatibility") != std::wstring::npos ||
        t.find(L"收藏夹") != std::wstring::npos ||
        t.find(L"add to favorites") != std::wstring::npos ||
        t.find(L"添加到收藏") != std::wstring::npos ||
        v.find(L"previousversion") != std::wstring::npos ||
        v == L"offline" || v.find(L"signature") != std::wstring::npos)
        return CtxMenuCategory::SystemExtra;

    return CtxMenuCategory::Software;
}

} // namespace pulse::ipc
