#include "../common/localization.h"
// shell_verbs.cpp — See shell_verbs.h for the contract.
#include "shell_verbs.h"

#include "../ipc/ctx_menu_util.h"

#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::app {

namespace {

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring RegReadString(HKEY root, const std::wstring& subkey, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring out(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, out.data(), &size) != ERROR_SUCCESS)
        return {};
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

bool RegValueExists(HKEY root, const std::wstring& subkey, const wchar_t* value) {
    return RegGetValueW(root, subkey.c_str(), value, RRF_RT_ANY, nullptr, nullptr,
                        nullptr) == ERROR_SUCCESS;
}

std::wstring StripMnemonics(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'&') {
            if (i + 1 < in.size() && in[i + 1] == L'&') {
                out += L'&';
                ++i;
            }
            continue;
        }
        out += in[i];
    }
    return out;
}

// "@shell32.dll,-8506" style indirect strings -> localized text.
std::wstring ResolveIndirect(const std::wstring& s) {
    if (s.empty() || s[0] != L'@') return s;
    wchar_t buf[512]{};
    if (SUCCEEDED(SHLoadIndirectString(s.c_str(), buf, ARRAYSIZE(buf), nullptr)) && buf[0])
        return buf;
    return {};
}

// Verbs that duplicate Pulse built-ins or are not user-facing.
bool IsSkippedStaticVerb(const std::wstring& lower, bool background) {
    if (lower == L"opennew" || lower == L"printto" || lower == L"find") return true;
    return ipc::IsBuiltinContextVerb(lower, background);
}

std::wstring KnownVerbDisplay(const std::wstring& lower) {
    if (lower == L"edit") return pulse::l10n::Get(pulse::l10n::StringId::ShellEdit).c_str();
    if (lower == L"print") return pulse::l10n::Get(pulse::l10n::StringId::ShellPrint).c_str();
    if (lower == L"preview") return pulse::l10n::Get(pulse::l10n::StringId::ShellPreview).c_str();
    if (lower == L"runas") return pulse::l10n::Get(pulse::l10n::StringId::ShellRunAs).c_str();
    return {};
}

bool FillStaticVerbFromKey(HKEY root, const std::wstring& verb_key, const std::wstring& verb,
                           bool allow_cascade, bool background, StaticVerb& out);
void CollectCascadeChildren(HKEY parent_root, const std::wstring& subcommands,
                            const std::wstring& ext_key, bool background,
                            std::vector<StaticVerb>& children);
void CollectVerbsFrom(HKEY root, const std::wstring& shell_key, std::vector<StaticVerb>& out,
                      bool allow_cascade, bool background,
                      std::unordered_set<std::wstring>* seen);

bool FillStaticVerbFromKey(HKEY root, const std::wstring& verb_key, const std::wstring& verb,
                           bool allow_cascade, bool background, StaticVerb& out) {
    // CommandStore and ExtendedSubCommandsKey can point back to an ancestor.
    thread_local std::vector<std::pair<HKEY, std::wstring>> active_keys;
    const auto key = std::make_pair(root, ToLower(verb_key));
    if (active_keys.size() >= static_cast<size_t>(ipc::kMaxMenuDepth) ||
        std::find(active_keys.begin(), active_keys.end(), key) != active_keys.end()) return false;
    active_keys.push_back(key);
    struct PopKey {
        decltype(active_keys)& keys;
        ~PopKey() { keys.pop_back(); }
    } pop{active_keys};
    const std::wstring lower = ToLower(verb);
    if (IsSkippedStaticVerb(lower, background)) return false;

    ipc::StaticVerbRegFlags flags;
    flags.legacy_disable = RegValueExists(root, verb_key, L"LegacyDisable");
    flags.programmatic = RegValueExists(root, verb_key, L"ProgrammaticAccessOnly");
    flags.extended = RegValueExists(root, verb_key, L"Extended");
    const std::wstring command = RegReadString(root, verb_key + L"\\command", nullptr);
    flags.has_command = !command.empty();
    flags.has_delegate_execute =
        !RegReadString(root, verb_key + L"\\command", L"DelegateExecute").empty();
    flags.has_explorer_command =
        !RegReadString(root, verb_key, L"ExplorerCommandHandler").empty();
    const std::wstring subcommands = RegReadString(root, verb_key, L"SubCommands");
    const std::wstring ext_key = RegReadString(root, verb_key, L"ExtendedSubCommandsKey");
    flags.has_subcommands = RegValueExists(root, verb_key, L"SubCommands");
    flags.has_extended_subcommands_key = !ext_key.empty();
    if (!ipc::KeepStaticVerb(flags)) return false;
    // The host asks IExplorerCommand for its actual title, state and children.
    if (flags.has_explorer_command) return false;
    // AppliesTo is an AQS condition only the shell can evaluate against the
    // item; Explorer hides the verb when it does not match. Not showing it is
    // the safe side (e.g. *\shell\UpdateEncryptionSettingsWork).
    if (RegValueExists(root, verb_key, L"AppliesTo")) return false;

    std::wstring display = ResolveIndirect(RegReadString(root, verb_key, L"MUIVerb"));
    if (display.empty()) display = RegReadString(root, verb_key, nullptr);
    if (!display.empty() && display[0] == L'@') display = ResolveIndirect(display);
    if (display.empty()) display = KnownVerbDisplay(lower);
    if (display.empty()) display = verb;

    out.verb = verb;
    out.display = StripMnemonics(display);
    out.mnemonic = ipc::MenuMnemonic(display);
    out.app_path.clear();
    // Delegated verbs may register an inert command fallback (Windows'
    // wallpaper verb points at Explorer.exe); ShellExecute must invoke
    // the registered handler rather than launch that fallback on the file.
    out.command = ipc::UseStaticVerbCommand(flags) ? command : std::wstring();
    out.icon = RegReadString(root, verb_key, L"Icon");
    out.children.clear();
    if (allow_cascade)
        CollectCascadeChildren(root, subcommands, ext_key, background, out.children);
    if ((flags.has_subcommands || flags.has_extended_subcommands_key) &&
        out.children.empty() && !flags.has_command)
        return false;
    return true;
}

void CollectCascadeChildren(HKEY parent_root, const std::wstring& subcommands,
                            const std::wstring& ext_key, bool background,
                            std::vector<StaticVerb>& children) {
    if (!subcommands.empty()) {
        std::wstring cur;
        auto flush = [&] {
            if (cur.empty()) return;
            StaticVerb child;
            const std::wstring store =
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\" +
                cur;
            if (FillStaticVerbFromKey(HKEY_LOCAL_MACHINE, store, cur, true, background, child) ||
                FillStaticVerbFromKey(HKEY_CURRENT_USER, store, cur, true, background, child))
                children.push_back(std::move(child));
            cur.clear();
        };
        for (wchar_t c : subcommands) {
            if (c == L' ' || c == L';' || c == L'\t') flush();
            else cur += c;
        }
        flush();
    }
    if (!ext_key.empty()) {
        std::wstring shell_key = ext_key;
        while (!shell_key.empty() && (shell_key.back() == L'\\' || shell_key.back() == L'/'))
            shell_key.pop_back();
        const std::wstring lower = ToLower(shell_key);
        if (lower.size() < 6 || lower.compare(lower.size() - 6, 6, L"\\shell") != 0)
            shell_key += L"\\shell";
        CollectVerbsFrom(parent_root, shell_key, children, true, background, nullptr);
    }
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [](const StaticVerb& child) {
                                      return child.children.empty() && child.command.empty() && child.app_path.empty();
                                  }),
                   children.end());
}

void CollectVerbsFrom(HKEY root, const std::wstring& shell_key, std::vector<StaticVerb>& out,
                      bool allow_cascade, bool background,
                      std::unordered_set<std::wstring>* seen) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, shell_key.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD name_len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        const std::wstring verb = name;
        const std::wstring lower = ToLower(verb);
        if (seen && !seen->insert(lower).second) continue;
        StaticVerb v;
        if (!FillStaticVerbFromKey(root, shell_key + L"\\" + verb, verb, allow_cascade,
                                   background, v)) {
            if (seen) seen->erase(lower);
            continue;
        }
        out.push_back(std::move(v));
    }
    RegCloseKey(key);
}

// First token of a shell command line = executable path.
std::wstring CommandExePath(const std::wstring& command) {
    if (command.empty()) return {};
    std::wstring exe;
    if (command[0] == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return {};
        exe = command.substr(1, end - 1);
    } else {
        exe = command.substr(0, command.find(L' '));
    }
    wchar_t expanded[MAX_PATH]{};
    if (ExpandEnvironmentStringsW(exe.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        exe = expanded;
    return exe;
}

// FileDescription from the exe's version resource ("AutoCAD Application" …).
// localized: the MUI resource of the UI language (记事本, as Explorer shows).
std::wstring ExeFriendlyName(const std::wstring& exe_path, bool localized = false) {
    DWORD ignored = 0;
    const DWORD size = localized ? GetFileVersionInfoSizeExW(FILE_VER_GET_LOCALISED, exe_path.c_str(), &ignored)
                                 : GetFileVersionInfoSizeW(exe_path.c_str(), nullptr);
    if (size == 0) return {};
    std::vector<uint8_t> data(size);
    if (localized ? !GetFileVersionInfoExW(FILE_VER_GET_LOCALISED, exe_path.c_str(), 0, size, data.data())
                  : !GetFileVersionInfoW(exe_path.c_str(), 0, size, data.data()))
        return {};
    struct LangCp { WORD lang; WORD cp; };
    LangCp* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &bytes) ||
        bytes < sizeof(LangCp))
        return {};
    wchar_t query[64];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               translations[0].lang, translations[0].cp);
    wchar_t* desc = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&desc), &len) &&
        desc && len > 1)
        return std::wstring(desc);
    return {};
}

std::wstring ExeBaseName(const std::wstring& exe) {
    auto pos = exe.find_last_of(L"\\/");
    std::wstring name = pos == std::wstring::npos ? exe : exe.substr(pos + 1);
    pos = name.find_last_of(L'.');
    if (pos != std::wstring::npos) name.resize(pos);
    return name;
}

// Resolves "acad.exe" from OpenWithList to a launchable path + friendly name.
bool ResolveOpenWithApp(const std::wstring& exe_name, StaticVerb& out) {
    std::wstring path = CommandExePath(RegReadString(
        HKEY_CLASSES_ROOT, L"Applications\\" + exe_name + L"\\shell\\open\\command",
        nullptr));
    if (path.empty()) {
        path = RegReadString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exe_name,
            nullptr);
    }
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    std::wstring friendly = ResolveIndirect(RegReadString(
        HKEY_CLASSES_ROOT, L"Applications\\" + exe_name, L"FriendlyAppName"));
    if (friendly.empty()) friendly = ExeFriendlyName(path);
    if (friendly.empty()) friendly = ExeBaseName(exe_name);
    out.display = pulse::l10n::Get(pulse::l10n::StringId::ShellOpenApp);
    const size_t placeholder = out.display.find(L"{app}");
    if (placeholder != std::wstring::npos) out.display.replace(placeholder, 5, friendly);
    out.app_path = path;
    out.icon = path;  // Explorer shows the program's own icon on these rows
    return true;
}

// ---- Row icons -------------------------------------------------------------

// HICON -> premultiplied BGRA (menu_icon.h). Icons with an alpha channel use
// it; classic ones take the AND mask. Monochrome icons are skipped.
common::MenuIconRef IconPixels(HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return nullptr;
    common::MenuIconRef result;
    BITMAP color{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(color), &color) &&
        color.bmWidth > 0 && color.bmHeight > 0 &&
        color.bmWidth <= static_cast<LONG>(common::kMaxMenuIconEdge) &&
        color.bmHeight <= static_cast<LONG>(common::kMaxMenuIconEdge)) {
        auto pixels = std::make_shared<common::MenuIconPixels>();
        pixels->width = static_cast<uint32_t>(color.bmWidth);
        pixels->height = static_cast<uint32_t>(color.bmHeight);
        const size_t count = static_cast<size_t>(pixels->width) * pixels->height;
        pixels->bgra.resize(count * 4u);
        std::vector<uint8_t> mask(count * 4u);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = color.bmWidth;
        bi.bmiHeader.biHeight = -color.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (HDC dc = CreateCompatibleDC(nullptr)) {
            const UINT rows = static_cast<UINT>(color.bmHeight);
            const bool ok = GetDIBits(dc, info.hbmColor, 0, rows, pixels->bgra.data(), &bi,
                                      DIB_RGB_COLORS) == color.bmHeight;
            const bool has_mask = info.hbmMask &&
                GetDIBits(dc, info.hbmMask, 0, rows, mask.data(), &bi, DIB_RGB_COLORS) ==
                    color.bmHeight;
            DeleteDC(dc);
            if (ok) {
                auto& bgra = pixels->bgra;
                bool any_alpha = false;
                for (size_t i = 3; i < bgra.size(); i += 4) any_alpha = any_alpha || bgra[i] != 0;
                if (!any_alpha) {
                    // 24bpp-style icon: the mask (white = transparent) is the alpha.
                    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                        const bool opaque = !has_mask || mask[i] == 0;
                        bgra[i + 3] = opaque ? 255 : 0;
                        if (!opaque) bgra[i] = bgra[i + 1] = bgra[i + 2] = 0;
                    }
                } else {
                    // GetIconInfo hands out straight alpha; D2D wants premultiplied.
                    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                        const uint32_t a = bgra[i + 3];
                        if (a == 255) continue;
                        for (size_t c = 0; c < 3; ++c)
                            bgra[i + c] = static_cast<uint8_t>((bgra[i + c] * a + 127u) / 255u);
                    }
                }
                result = std::move(pixels);
            }
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return result;
}

// Menu icon edge in pixels: 16 DIP at the system DPI (one size per process;
// the menu scales the few-percent mismatch on a mixed-DPI desk).
uint32_t MenuIconEdgePx() {
    int dpi = 96;
    if (HDC screen = GetDC(nullptr)) {
        dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    const int px = MulDiv(16, dpi > 0 ? dpi : 96, 96);
    return static_cast<uint32_t>(std::clamp(px, 16, static_cast<int>(common::kMaxMenuIconEdge)));
}

// "C:\...\app.exe,-101" / "%ProgramFiles%\app.exe" / quoted -> file + index.
bool ParseIconReference(const std::wstring& reference, std::wstring& file, int& index) {
    if (reference.empty() || reference.size() >= MAX_PATH * 2 || reference[0] == L'@') return false;
    wchar_t buffer[MAX_PATH * 2]{};
    wcsncpy_s(buffer, reference.c_str(), _TRUNCATE);
    index = PathParseIconLocationW(buffer);
    PathUnquoteSpacesW(buffer);
    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(buffer, expanded, ARRAYSIZE(expanded));
    file = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : buffer;
    return !file.empty();
}

common::MenuIconRef LoadMenuIcon(const std::wstring& reference, uint32_t edge) {
    std::wstring file;
    int index = 0;
    if (!ParseIconReference(reference, file, index)) return nullptr;
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr,
                                 MAKELONG(edge, edge))) || !icon)
        return nullptr;
    common::MenuIconRef pixels = IconPixels(icon);
    DestroyIcon(icon);
    return pixels;
}

// Process-wide cache: a few dozen distinct references across all types, and
// misses (no such file, no icon) are remembered too.
common::MenuIconRef CachedMenuIcon(const std::wstring& reference) {
    // Never destroyed: SeedShellVerbCache's detached thread can still be here
    // while exit() runs static destructors, and a destroyed map crashes it.
    static std::mutex& mutex = *new std::mutex;
    static auto& cache = *new std::unordered_map<std::wstring, common::MenuIconRef>;
    static uint32_t edge = 0;
    const std::wstring key = ToLower(reference);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (edge == 0) edge = MenuIconEdgePx();
        if (const auto found = cache.find(key); found != cache.end()) return found->second;
    }
    common::MenuIconRef pixels = LoadMenuIcon(reference, edge);
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 1024) cache.clear();
    cache.emplace(key, pixels);
    return pixels;
}

void CollectOpenWith(const std::wstring& ext_lower, const std::wstring& default_exe_lower,
                     std::vector<StaticVerb>& out) {
    const std::wstring list_key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
        ext_lower + L"\\OpenWithList";
    const std::wstring mru = RegReadString(HKEY_CURRENT_USER, list_key, L"MRUList");
    size_t added = 0;
    for (wchar_t slot : mru) {
        if (added >= 3) break;
        const wchar_t value_name[2] = { slot, L'\0' };
        const std::wstring exe_name = RegReadString(HKEY_CURRENT_USER, list_key, value_name);
        if (exe_name.empty()) continue;
        StaticVerb v;
        if (!ResolveOpenWithApp(exe_name, v)) continue;
        if (!default_exe_lower.empty() && ToLower(v.app_path) == default_exe_lower)
            continue; // duplicates the built-in 打开 row
        v.verb = L"__openwith";
        out.push_back(std::move(v));
        ++added;
    }
}

} // namespace

std::vector<StaticVerb> EnumerateStaticVerbs(const std::wstring& ext) {
    std::vector<StaticVerb> out;
    if (ipc::IsLocationVerbKey(ext)) {
        std::unordered_set<std::wstring> seen;
        const bool background = ext == ipc::kBackgroundVerbKey;
        if (background) {
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell", out, true,
                             true, &seen);
        } else if (ext == ipc::kDriveVerbKey) {
            // A drive's menu reads Drive and Folder only (as the handler side).
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Drive\\shell", out, true, false, &seen);
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Folder\\shell", out, true, false, &seen);
        } else {
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Directory\\shell", out, true, false, &seen);
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Folder\\shell", out, true, false, &seen);
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shell", out, true,
                             false, &seen);
        }
        return DedupeStaticVerbs(std::move(out), {}, 48);
    }
    if (ext.size() < 2 || ext[0] != L'.') return out;
    const std::wstring ext_lower = ToLower(ext);

    // The ProgID the shell itself resolves for the type (its menu uses that
    // one). Hand-reading UserChoice / HKCR\.ext diverges from it: .dwg with an
    // empty UserChoice resolved to CADFile in Explorer but AutoCAD.Drawing.26
    // here, so Pulse showed 打印 and a signature verb Explorer did not.
    std::wstring progid;
    {
        wchar_t resolved[256]{};
        DWORD cch = ARRAYSIZE(resolved);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, ext_lower.c_str(),
                                        nullptr, resolved, &cch)) &&
            resolved[0] && _wcsicmp(resolved, ext_lower.c_str()) != 0)
            progid = resolved;
    }
    if (progid.empty()) {
        progid = RegReadString(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
                ext_lower + L"\\UserChoice",
            L"ProgId");
    }
    if (progid.empty())
        progid = RegReadString(HKEY_CLASSES_ROOT, ext_lower, nullptr);

    // Explorer's association order for a file; a verb name is taken from the
    // first class that defines it.
    std::unordered_set<std::wstring> seen;
    std::wstring default_exe_lower;
    if (!progid.empty()) {
        default_exe_lower = ToLower(CommandExePath(RegReadString(
            HKEY_CLASSES_ROOT, progid + L"\\shell\\open\\command", nullptr)));
        CollectVerbsFrom(HKEY_CLASSES_ROOT, progid + L"\\shell", out, true, false, &seen);
    }
    CollectVerbsFrom(HKEY_CLASSES_ROOT,
                     L"SystemFileAssociations\\" + ext_lower + L"\\shell", out, true, false,
                     &seen);
    const std::wstring perceived = RegReadString(HKEY_CLASSES_ROOT, ext_lower, L"PerceivedType");
    if (!perceived.empty()) {
        CollectVerbsFrom(HKEY_CLASSES_ROOT,
                         L"SystemFileAssociations\\" + ToLower(perceived) + L"\\shell", out,
                         true, false, &seen);
    }
    // Verbs every file gets (解锁占用, 使用 ToDesk 快传文件, 添加到收藏夹 …).
    CollectVerbsFrom(HKEY_CLASSES_ROOT, L"*\\shell", out, true, false, &seen);
    CollectVerbsFrom(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shell", out, true, false, &seen);

    // Programs and scripts get no 打开方式 in Explorer.
    if (!ipc::OffersOpenWith(ext_lower)) return out;
    CollectOpenWith(ext_lower, default_exe_lower, out);

    StaticVerb open_as;
    open_as.verb = L"openas";
    open_as.display = pulse::l10n::Get(pulse::l10n::StringId::ShellOpenWith);
    // Seeding can run in a process without resources (the installer); an empty
    // row is dropped by the menu, which hid 打开方式 completely.
    if (open_as.display.empty()) open_as.display = L"Open with...";
    out.push_back(std::move(open_as));
    return out;
}

std::vector<StaticVerb> DedupeStaticVerbs(std::vector<StaticVerb> verbs,
                                          const std::vector<std::wstring>& builtin_texts,
                                          size_t cap) {
    std::vector<StaticVerb> out;
    std::vector<std::wstring> seen;
    for (const auto& b : builtin_texts) seen.push_back(ToLower(b));
    for (auto& v : verbs) {
        if (v.display.empty()) continue;
        const std::wstring key = ToLower(v.display);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(std::move(v));
        if (out.size() >= cap) break;
    }
    return out;
}

namespace {

void PutU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4);
}
void PutW(std::vector<uint8_t>& buf, const std::wstring& s) {
    PutU32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(s.data()),
               reinterpret_cast<const uint8_t*>(s.data() + s.size()));
}
bool GetU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (p + 4 > end) return false;
    memcpy(&v, p, 4);
    p += 4;
    return true;
}
bool GetW(const uint8_t*& p, const uint8_t* end, std::wstring& s) {
    uint32_t n = 0;
    if (!GetU32(p, end, n) || n > 4096) return false;
    if (p + n * 2 > end) return false;
    s.assign(reinterpret_cast<const wchar_t*>(p), n);
    p += n * 2;
    return true;
}

// v3 appends each verb's access key after its command. v4 keeps that layout
// but changes the content (shell-resolved ProgID, * / AllFilesystemObjects /
// PerceivedType verbs), so older files are discarded and reseeded.
// v5: drive verbs from Drive + Folder only; no 打开方式 for programs.
// v6: a DelegateExecute verb keeps no command (setdesktopwallpaper's
// placeholder Explorer.exe), so v5 files with that command are discarded.
// v7: native ExplorerCommandHandler titles replace registry-name fallback rows.
constexpr uint32_t kStaticVerbCacheVersion = 9;
constexpr uint32_t kStaticVerbCacheMinVersion = 9;

void PutVerb(std::vector<uint8_t>& buf, const StaticVerb& v, int depth) {
    PutW(buf, v.verb);
    PutW(buf, v.display);
    PutW(buf, v.app_path);
    PutW(buf, v.command);
    PutU32(buf, static_cast<uint32_t>(v.mnemonic));
    const uint32_t n = depth >= ipc::kMaxMenuDepth ? 0 : static_cast<uint32_t>(
        (std::min)(v.children.size(), static_cast<size_t>(ipc::kMaxSubmenuChildren)));
    PutU32(buf, n);
    for (uint32_t i = 0; i < n; ++i) PutVerb(buf, v.children[i], depth + 1);
}

bool GetVerb(const uint8_t*& p, const uint8_t* end, StaticVerb& v, int depth, uint32_t version) {
    if (!GetW(p, end, v.verb) || !GetW(p, end, v.display) || !GetW(p, end, v.app_path) ||
        !GetW(p, end, v.command))
        return false;
    v.mnemonic = 0;
    if (version >= 3) {
        uint32_t key = 0;
        if (!GetU32(p, end, key) || key > 0xFFFF) return false;
        v.mnemonic = static_cast<wchar_t>(key);
    }
    uint32_t n = 0;
    if (!GetU32(p, end, n) || n > 16 || (depth >= ipc::kMaxMenuDepth && n != 0)) return false;
    v.children.clear();
    v.children.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        StaticVerb child;
        if (!GetVerb(p, end, child, depth + 1, version)) return false;
        v.children.push_back(std::move(child));
    }
    return true;
}

} // namespace

void ResolveStaticVerbIcons(std::vector<StaticVerb>& verbs) {
    for (auto& verb : verbs) {
        if (!verb.icon.empty() && !verb.icon_pixels) verb.icon_pixels = CachedMenuIcon(verb.icon);
        ResolveStaticVerbIcons(verb.children);
    }
}

std::wstring MachineStaticVerbCachePath() {
    wchar_t dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, dir))) return {};
    std::wstring path = std::wstring(dir) + L"\\Pulse";
    CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\shell-verbs.bin";
}

bool LoadMachineStaticVerbCache(std::unordered_map<std::wstring, std::vector<StaticVerb>>& out) {
    out.clear();
    const std::wstring path = MachineStaticVerbCachePath();
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 16 || sz.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
    DWORD r = 0;
    const bool ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr) &&
        r == buf.size();
    CloseHandle(h);
    if (!ok) return false;
    const uint8_t* p = buf.data();
    const uint8_t* end = p + buf.size();
    if (p + 8 > end || memcmp(p, "PSVC", 4) != 0) return false;
    p += 4;
    uint32_t ver = 0, count = 0;
    if (!GetU32(p, end, ver) || ver < kStaticVerbCacheMinVersion || ver > kStaticVerbCacheVersion ||
        !GetU32(p, end, count) ||
        count > 8000)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::wstring ext;
        uint32_t nverb = 0;
        if (!GetW(p, end, ext) || !GetU32(p, end, nverb) || nverb > 64) return false;
        std::vector<StaticVerb> verbs;
        verbs.reserve(nverb);
        for (uint32_t k = 0; k < nverb; ++k) {
            StaticVerb v;
            if (!GetVerb(p, end, v, 0, ver)) return false;
            verbs.push_back(std::move(v));
        }
        if (!ext.empty()) out.emplace(std::move(ext), std::move(verbs));
    }
    return true;
}

bool SaveMachineStaticVerbCache(
    const std::unordered_map<std::wstring, std::vector<StaticVerb>>& cache) {
    const std::wstring path = MachineStaticVerbCachePath();
    if (path.empty()) return false;
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), {'P', 'S', 'V', 'C'});
    PutU32(buf, kStaticVerbCacheVersion);
    PutU32(buf, static_cast<uint32_t>(cache.size()));
    for (const auto& [ext, verbs] : cache) {
        PutW(buf, ext);
        PutU32(buf, static_cast<uint32_t>(verbs.size()));
        for (const auto& v : verbs) PutVerb(buf, v, 0);
    }
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr) &&
        w == buf.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool SeedMachineStaticVerbCache() {
    std::unordered_map<std::wstring, std::vector<StaticVerb>> cache;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < 8000; ++i) {
        wchar_t name[128];
        DWORD name_len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        if (name_len < 2 || name[0] != L'.') continue;
        std::wstring ext = ToLower(name);
        auto verbs = EnumerateStaticVerbs(ext);
        if (!verbs.empty()) cache.emplace(std::move(ext), std::move(verbs));
    }
    RegCloseKey(key);
    return SaveMachineStaticVerbCache(cache);
}

common::MenuIconRef FileTypeMenuIcon(const std::wstring& ext) {
    // Never destroyed, for the same reason as CachedMenuIcon's cache.
    static std::mutex& mutex = *new std::mutex;
    static auto& cache = *new std::unordered_map<std::wstring, common::MenuIconRef>;
    const std::wstring key = ToLower(ext);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto found = cache.find(key); found != cache.end()) return found->second;
    }
    common::MenuIconRef pixels;
    SHFILEINFOW info{};
    if (!key.empty() && SHGetFileInfoW(key.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                                       SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON) && info.hIcon) {
        pixels = IconPixels(info.hIcon);
        DestroyIcon(info.hIcon);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 256) cache.clear();
    cache.emplace(key, pixels);
    return pixels;
}

std::wstring ProgramFriendlyName(const std::wstring& exe_path) {
    if (exe_path.empty()) return {};
    const size_t slash = exe_path.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? exe_path : exe_path.substr(slash + 1);
    // Same order as the open-with rows (the registered name, the version
    // resource, the file name), but the version resource in the UI language:
    // the name Explorer's 打开方式 shows — 记事本, not "Notepad".
    std::wstring friendly = ResolveIndirect(RegReadString(
        HKEY_CLASSES_ROOT, L"Applications\\" + file, L"FriendlyAppName"));
    if (friendly.empty()) friendly = ExeFriendlyName(exe_path, true);
    if (friendly.empty()) friendly = ExeFriendlyName(exe_path);
    if (friendly.empty()) friendly = ExeBaseName(file);
    return friendly;
}

common::MenuIconRef ProgramMenuIcon(const std::wstring& reference) {
    return reference.empty() ? nullptr : CachedMenuIcon(reference);
}

} // namespace pulse::app
