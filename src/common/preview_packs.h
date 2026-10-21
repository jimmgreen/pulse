// preview_packs.h - Optional preview packs (FFmpeg media, modern image codecs).
//
// A pack is a set of helper executables Pulse never ships in the installer.
// The user installs one from Settings > Preview packs (or from the prompt
// Quick Look shows when Windows lacks a decoder); afterwards the preview host
// calls it only when the native path - WIC, Media Foundation, the shell
// provider - could not make the preview. Without a pack nothing changes.
//
// Layout under %LOCALAPPDATA%\Pulse\packs:
//   packs.json                         shared user settings, written by the app
//   <key>\installed.json               x64 {"version":"7.1.1","dir":"7.1.1"}
//   <key>\<dir>\ffmpeg.exe ...         x64 pack files (existing layout)
//   arm64\<key>\installed.json         ARM64 installation, separate from x64
//   arm64\<key>\<dir>\ffmpeg.exe ...   ARM64 pack files
//
// Header-only on purpose: the preview host, the app and the tests are
// separate executables and must not gain link dependencies.
#pragma once

#include "json_utils.h"
#include "pack_architecture.h"
#include "utf8_file.h"
#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace pulse::packs {

enum class PackId : uint32_t { Media = 0, Images = 1, Raw = 2, Archives = 3 };
inline constexpr uint32_t kPackCount = 4;

// Directory and settings-key name of a pack.
inline const wchar_t* PackKey(PackId id) {
    switch (id) {
    case PackId::Media: return L"ffmpeg";
    case PackId::Images: return L"images";
    case PackId::Raw: return L"raw";
    case PackId::Archives: return L"archive";
    }
    return L"";
}

// Main executable of a pack; its siblings live in the same directory.
inline const wchar_t* PackMainTool(PackId id) {
    switch (id) {
    case PackId::Media: return L"ffmpeg.exe";
    case PackId::Images: return L"pulse-imgpack.exe";
    case PackId::Raw: return L"pulse-rawpack.exe";
    case PackId::Archives: return L"7z.exe";
    }
    return L"";
}

// %LOCALAPPDATA%\Pulse\packs, empty when the variable is unavailable. Tests and
// --test-instance runs redirect LOCALAPPDATA, which isolates packs as well.
inline std::wstring PacksSharedRoot() {
    wchar_t root[32768];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    if (!count || count >= std::size(root)) return {};
    return std::wstring(root, count) + L"\\Pulse\\packs";
}

inline std::wstring PacksRoot(Architecture architecture = ProcessArchitecture()) {
    const std::wstring root = PacksSharedRoot();
    if (root.empty()) return {};
    switch (architecture) {
    case Architecture::X64: return root;
    case Architecture::Arm64: return root + L"\\arm64";
    }
    return {};
}

inline std::wstring PacksSettingsPath() {
    // Preferences survive switching between native ARM64 and emulated x64 Pulse.
    const std::wstring root = PacksSharedRoot();
    return root.empty() ? std::wstring{} : root + L"\\packs.json";
}

inline bool IsRegularFile(const std::wstring& path) {
    const DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline std::wstring DirectoryOf(const std::wstring& file) {
    const size_t slash = file.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : file.substr(0, slash);
}

// User settings (packs.json). Flat keys so json_utils' key search is exact.
struct PackSettings {
    bool enabled[kPackCount] = {true, true, true, true};
    // Media pack only: an FFmpeg the user already has (path to ffmpeg.exe).
    bool use_custom_ffmpeg = false;
    std::wstring custom_ffmpeg;
    bool auto_update = true;        // 自动更新预览增强包, see PackAutoUpdateWanted
    uint32_t download_source = 0;   // 0 automatic, 1 GitHub, 2 accelerated mirror
    bool remove_on_uninstall = true;
};

inline PackSettings ParsePackSettings(const std::wstring& text) {
    PackSettings s;
    s.enabled[0] = json::ExtractBool(text, L"ffmpeg_enabled", true);
    s.enabled[1] = json::ExtractBool(text, L"images_enabled", true);
    s.enabled[2] = json::ExtractBool(text, L"raw_enabled", true);
    s.enabled[3] = json::ExtractBool(text, L"archive_enabled", true);
    s.use_custom_ffmpeg = json::ExtractBool(text, L"ffmpeg_use_custom", false);
    s.custom_ffmpeg = json::ExtractString(text, L"ffmpeg_custom_path");
    s.auto_update = json::ExtractBool(text, L"auto_update", true);
    const int source = json::ExtractInt(text, L"download_source", 0);
    s.download_source = source >= 0 && source <= 2 ? static_cast<uint32_t>(source) : 0u;
    s.remove_on_uninstall = json::ExtractBool(text, L"remove_on_uninstall", true);
    return s;
}

inline std::wstring SerializePackSettings(const PackSettings& s) {
    std::wstring path;
    json::Escape(s.custom_ffmpeg, path);
    std::wstring out = L"{\n";
    out += L"  \"ffmpeg_enabled\": " + std::wstring(s.enabled[0] ? L"true" : L"false") + L",\n";
    out += L"  \"images_enabled\": " + std::wstring(s.enabled[1] ? L"true" : L"false") + L",\n";
    out += L"  \"raw_enabled\": " + std::wstring(s.enabled[2] ? L"true" : L"false") + L",\n";
    out += L"  \"archive_enabled\": " + std::wstring(s.enabled[3] ? L"true" : L"false") + L",\n";
    out += L"  \"ffmpeg_use_custom\": " + std::wstring(s.use_custom_ffmpeg ? L"true" : L"false") + L",\n";
    out += L"  \"ffmpeg_custom_path\": \"" + path + L"\",\n";
    out += L"  \"auto_update\": " + std::wstring(s.auto_update ? L"true" : L"false") + L",\n";
    out += L"  \"download_source\": " + std::to_wstring(s.download_source) + L",\n";
    out += L"  \"remove_on_uninstall\": " + std::wstring(s.remove_on_uninstall ? L"true" : L"false") + L"\n";
    out += L"}\n";
    return out;
}

inline PackSettings LoadPackSettings() {
    std::wstring text;
    const std::wstring path = PacksSettingsPath();
    if (path.empty() || !ReadUtf8File(path, text)) return {};
    return ParsePackSettings(text);
}

// Creates every missing directory of an absolute path.
inline bool CreateDirectoryChain(const std::wstring& directory) {
    // Intermediate failures are expected (drive roots, \\server on UNC paths);
    // only the final directory has to exist.
    for (size_t at = directory.find_first_of(L"\\/", 3); ; at = directory.find_first_of(L"\\/", at + 1)) {
        CreateDirectoryW(directory.substr(0, at).c_str(), nullptr);
        if (at == std::wstring::npos) break;
    }
    const DWORD attrs = GetFileAttributesW(directory.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool SavePackSettings(const PackSettings& settings) {
    const std::wstring path = PacksSettingsPath();
    if (path.empty() || !CreateDirectoryChain(DirectoryOf(path))) return false;
    return WriteUtf8FileAtomic(path, SerializePackSettings(settings));
}

struct InstalledPack {
    bool present = false;
    std::wstring version;
    std::wstring directory;   // absolute directory holding the pack's files
};

// The version directory <base>\installed.json names (base is <root>\<key>),
// and its version; empty when there is none.
inline std::wstring InstalledPackDirectory(const std::wstring& base, std::wstring* version = nullptr) {
    std::wstring text;
    if (base.empty() || !ReadUtf8File(base + L"\\installed.json", text)) return {};
    const std::wstring dir = json::ExtractString(text, L"dir");
    // A version directory name, never a path: installed.json is user-writable.
    if (dir.empty() || dir.find_first_of(L"\\/:") != std::wstring::npos || dir == L"." || dir == L"..")
        return {};
    if (version) *version = json::ExtractString(text, L"version");
    return base + L"\\" + dir;
}

// Reads <root>\<key>\installed.json and checks the main tool is there.
inline InstalledPack ReadInstalledPack(PackId id, Architecture architecture = ProcessArchitecture()) {
    InstalledPack pack;
    const std::wstring root = PacksRoot(architecture);
    if (root.empty()) return pack;
    pack.directory = InstalledPackDirectory(root + L"\\" + PackKey(id), &pack.version);
    pack.present = !pack.directory.empty() && IsRegularFile(pack.directory + L"\\" + PackMainTool(id));
    return pack;
}

namespace detail {
// "1.0.2" as numbers; false for anything but one to eight dotted decimal
// parts (installed.json is user-writable).
inline bool PackVersionParts(std::wstring_view text, uint32_t (&parts)[8], size_t& count) {
    count = 0;
    size_t at = 0;
    while (count < 8) {
        uint64_t value = 0;
        const size_t start = at;
        for (; at < text.size() && text[at] >= L'0' && text[at] <= L'9'; ++at) {
            value = value * 10 + static_cast<uint64_t>(text[at] - L'0');
            if (value > 0xFFFFFFFFull) return false;
        }
        if (at == start) return false;
        parts[count++] = static_cast<uint32_t>(value);
        if (at == text.size()) return true;
        if (text[at++] != L'.') return false;
    }
    return false;
}
} // namespace detail

// Whether the catalog's release replaces the installed one. Dotted numbers
// compare part by part ("1.0.10" > "1.0.9", "1.0" == "1.0.0"). An installed
// version that is no such number (hand-edited, or a test build) is replaced
// too; a catalog version that is none is never offered.
inline bool PackUpdateAvailable(std::wstring_view installed, std::wstring_view catalog) {
    uint32_t want[8]{}, have[8]{};
    size_t want_count = 0, have_count = 0;
    if (!detail::PackVersionParts(catalog, want, want_count)) return false;
    if (!detail::PackVersionParts(installed, have, have_count)) return true;
    for (size_t i = 0; i < 8; ++i) {
        const uint32_t a = i < have_count ? have[i] : 0, b = i < want_count ? want[i] : 0;
        if (a != b) return b > a;
    }
    return false;
}

// Whether a background update (Settings > 自动更新预览增强包) replaces an
// installed pack with the catalog's release: only a pack the preview host
// can use - switched on, and for FFmpeg not set aside for the user's own
// build - and only when the catalog is newer. The rest wait for 更新到.
inline bool PackAutoUpdateWanted(PackId id, const PackSettings& settings, const InstalledPack& installed,
                                 std::wstring_view catalog_version) {
    if (!settings.auto_update || !installed.present || !settings.enabled[static_cast<uint32_t>(id)]) return false;
    if (id == PackId::Media && settings.use_custom_ffmpeg) return false;
    return PackUpdateAvailable(installed.version, catalog_version);
}

// Where a pack's tools come from right now.
enum class ToolSource : uint32_t { None = 0, Pack = 1, Custom = 2 };

struct ResolvedPack {
    ToolSource source = ToolSource::None;
    std::wstring directory;    // directory holding the tools
};

inline ResolvedPack ResolvePackUncached(PackId id) {
    ResolvedPack out;
    const PackSettings settings = LoadPackSettings();
    if (!settings.enabled[static_cast<uint32_t>(id)]) return out;
    if (id == PackId::Media && settings.use_custom_ffmpeg) {
        if (IsRegularFile(settings.custom_ffmpeg)) {
            out.source = ToolSource::Custom;
            out.directory = DirectoryOf(settings.custom_ffmpeg);
        }
        // An explicitly chosen source must not silently change if its file moves.
        return out;
    }
    const InstalledPack pack = ReadInstalledPack(id);
    if (pack.present) {
        out.source = ToolSource::Pack;
        out.directory = pack.directory;
    }
    return out;
}

// Write times of shared packs.json and native <key>\installed.json. Settings
// and the installer write one of them on every change, so comparing this stamp
// lets a change made in Settings reach the very next request.
inline uint64_t PackStamp(PackId id) {
    const std::wstring root = PacksRoot();
    if (root.empty()) return 0;
    uint64_t stamp = 0;
    for (const std::wstring& file : {PacksSettingsPath(), root + L"\\" + PackKey(id) + L"\\installed.json"}) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const uint64_t time = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)
            ? (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime
            : 1;
        stamp = stamp * 1000003u + time;
    }
    return stamp;
}

// The preview host asks this for every candidate file, so the answer is
// cached: two attribute reads per call, the settings and pack files only when
// they changed or every few seconds (a custom ffmpeg.exe may have been deleted).
inline ResolvedPack ResolvePack(PackId id) {
    struct Slot { ULONGLONG checked = 0; uint64_t stamp = 0; ResolvedPack value; };
    static Slot slots[kPackCount];
    static std::mutex lock;
    constexpr ULONGLONG kRecheckMs = 3000;
    const ULONGLONG now = GetTickCount64();
    const uint64_t stamp = PackStamp(id);
    std::lock_guard<std::mutex> guard(lock);
    Slot& slot = slots[static_cast<uint32_t>(id)];
    if (slot.checked == 0 || now - slot.checked >= kRecheckMs || stamp != slot.stamp) {
        slot.value = ResolvePackUncached(id);
        slot.checked = now ? now : 1;
        slot.stamp = stamp;
    }
    return slot.value;
}

// Full path of one of a pack's executables, empty when unavailable.
inline std::wstring PackToolPath(PackId id, std::wstring_view exe) {
    const ResolvedPack pack = ResolvePack(id);
    if (pack.source == ToolSource::None) return {};
    std::wstring path = pack.directory + L"\\" + std::wstring(exe);
    return IsRegularFile(path) ? path : std::wstring{};
}

} // namespace pulse::packs
