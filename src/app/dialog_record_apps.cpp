#include "dialog_record_apps.h"
#include "shell_verbs.h"
#include "../shell_bridge/dialog_application_identity.h"
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace pulse::app {
namespace {

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring RunningImage(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[32768]{};
    DWORD length = ARRAYSIZE(buffer);
    std::wstring image;
    if (QueryFullProcessImageNameW(process, 0, buffer, &length)) image.assign(buffer, length);
    CloseHandle(process);
    return image;
}

// HKCU then HKLM "App Paths\<exe>" default value, unquoted and expanded.
std::wstring AppPath(const std::wstring& basename) {
    const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + basename;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            // RegOpenKeyEx with an explicit view: RRF_SUBKEY_WOW64* needs a newer target than Windows 8.1.
            HKEY opened = nullptr;
            if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | view, &opened) != ERROR_SUCCESS) continue;
            wchar_t raw[MAX_PATH * 2]{};
            DWORD bytes = sizeof(raw) - sizeof(wchar_t);
            const LSTATUS read = RegGetValueW(opened, nullptr, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                                              nullptr, raw, &bytes);
            RegCloseKey(opened);
            if (read != ERROR_SUCCESS) continue;
            PathUnquoteSpacesW(raw);
            wchar_t expanded[MAX_PATH * 2]{};
            const DWORD n = ExpandEnvironmentStringsW(raw, expanded, ARRAYSIZE(expanded));
            std::wstring path = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : raw;
            const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                SameName(shell_bridge::application_identity::ExecutableBasename(path), basename))
                return path;
        }
    }
    return {};
}

} // namespace

uint32_t DialogRecordIconEdge() {
    int dpi = 96;
    if (HDC screen = GetDC(nullptr)) { dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen); }
    return static_cast<uint32_t>(std::clamp(MulDiv(32, dpi > 0 ? dpi : 96, 96), 16, static_cast<int>(common::kMaxMenuIconEdge)));
}

std::unordered_map<uint64_t, DialogRecordApp> ResolveDialogRecordApps(
    const std::vector<DialogRecordAppQuery>& queries, uint32_t icon_edge) {
    std::unordered_map<uint64_t, DialogRecordApp> out;
    if (queries.empty()) return out;
    // One pass over the running processes: only names a record asks for are opened.
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            for (const auto& query : queries) {
                if (out.contains(query.image_id) || !SameName(entry.szExeFile, query.basename)) continue;
                const auto image = RunningImage(entry.th32ProcessID);
                if (!image.empty() && shell_bridge::application_identity::ImageHash(image) == query.image_id)
                    out[query.image_id] = {image, {}, nullptr, true};
            }
        }
        CloseHandle(snapshot);
    }
    for (const auto& query : queries) {
        if (out.contains(query.image_id)) continue;
        auto path = AppPath(query.basename);
        const bool verified = !path.empty() && shell_bridge::application_identity::ImageHash(path) == query.image_id;
        out[query.image_id] = {std::move(path), {}, nullptr, verified};
    }
    for (auto& [id, app] : out) {
        if (app.path.empty()) continue;
        app.name = ProgramFriendlyName(app.path);
        // ",0": a comma in the path must not be read as the icon index.
        app.icon = common::LoadIconPixels(app.path + L",0", icon_edge);
    }
    return out;
}

} // namespace pulse::app
