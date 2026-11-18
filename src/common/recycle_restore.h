#pragma once

#include "current_user_security.h"
#include "path_utils.h"
#include "recycle_index.h"
#include "../shell_host/recycle_restore_index.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>

namespace pulse::recycle {

inline std::wstring CanonPath(std::wstring p) {
    p = pulse::path::StripExtendedPathPrefix(p);
    for (auto& c : p) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(towupper(c));
    }
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

inline bool ReadRecycleOriginal(const std::wstring& i_path, std::wstring& original) {
    original.clear();
    pulse::recycle::IndexRecord record;
    if (!pulse::recycle::ReadIndex(i_path, record)) return false;
    original = std::move(record.original_path);
    return true;
}

// \\?\ form for the Win32 calls below: this host has no longPathAware
// manifest, and restored items are often deeper than MAX_PATH.
inline std::wstring ExtendedPath(const std::wstring& path) {
    std::wstring plain = pulse::path::StripExtendedPathPrefix(path);
    std::replace(plain.begin(), plain.end(), L'/', L'\\');
    if (plain.size() > 2 && plain[0] == L'\\' && plain[1] == L'\\')
        return L"\\\\?\\UNC\\" + plain.substr(2);
    return L"\\\\?\\" + plain;
}

// Length of the "\\?\C:\" or "\\?\UNC\server\share\" root of an ExtendedPath.
inline size_t ExtendedRootLength(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) != 0) return 7;
    const size_t server = path.find(L'\\', 8);
    const size_t share = server == std::wstring::npos ? server : path.find(L'\\', server + 1);
    return share == std::wstring::npos ? path.size() : share + 1;
}

// The Shell's Restore recreates the original folder when it is gone.
inline bool EnsureParentFolder(const std::wstring& path, size_t root) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 <= root) return true;
    const std::wstring parent = path.substr(0, slash);
    const DWORD attributes = GetFileAttributesW(parent.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!EnsureParentFolder(parent, root)) return false;
    return CreateDirectoryW(parent.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// "name (2).ext": the next free name beside a taken one, as the Shell's
// "keep both" names it.
inline std::wstring FreeSiblingName(const std::wstring& path, bool folder) {
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring dir = path.substr(0, slash + 1);
    const std::wstring name = path.substr(slash + 1);
    size_t dot = folder ? std::wstring::npos : name.find_last_of(L'.');
    if (dot == 0) dot = std::wstring::npos;
    const std::wstring stem = name.substr(0, dot);
    const std::wstring extension = dot == std::wstring::npos ? L"" : name.substr(dot);
    for (int n = 2; n < 10000; ++n) {
        std::wstring candidate = dir + stem + L" (" + std::to_wstring(n) + L")" + extension;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return {};
}

// The production caller validates the current user's recycle-root identity
// before reaching this step. Tests use only their own isolated $I/$R fixtures.
// Restores like the Shell's Restore: a missing original folder is recreated,
// long paths work, and when the original name is taken again both are kept by
// restoring under the next free "name (N)" name.
inline bool RestoreSelectedRecyclePayload(const std::wstring& payload, std::wstring& error, bool exact_destination = false) {
    std::wstring index = payload;
    const size_t slash = index.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 2 >= index.size()) return false;
    index[slash + 2] = L'I';
    std::wstring original;
    if (!ReadRecycleOriginal(index, original)) {
        error = L"无法读取所选回收站条目";
        return false;
    }
    const std::wstring source = ExtendedPath(payload);
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = L"回收站中未找到该项";
        return false;
    }
    const bool folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring dest = ExtendedPath(original);
    bool moved = false;
    if (EnsureParentFolder(dest, ExtendedRootLength(dest))) {
        // A name taken between the check and the move just moves on to the next one.
        for (int attempt = 0; !moved && !dest.empty() && attempt < 4; ++attempt) {
            if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (exact_destination) break;
                dest = FreeSiblingName(dest, folder);
            }
            if (dest.empty()) break;
            moved = MoveFileExW(source.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
            if (!moved) {
                const DWORD move_error = GetLastError();
                if (exact_destination || (move_error != ERROR_ALREADY_EXISTS && move_error != ERROR_FILE_EXISTS)) break;
            }
        }
    }
    if (!moved) {
        error = L"还原失败 | " + pulse::path::StripExtendedPathPrefix(original);
        return false;
    }
    DeleteFileW(ExtendedPath(index).c_str());
    // Explorer windows on the original folder and on the Recycle Bin update.
    const std::wstring shown = pulse::path::StripExtendedPathPrefix(dest);
    const std::wstring gone = pulse::path::StripExtendedPathPrefix(source);
    SHChangeNotify(folder ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, shown.c_str(), nullptr);
    SHChangeNotify(folder ? SHCNE_RMDIR : SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, gone.c_str(), nullptr);
    return true;
}

inline bool RestoreOneFromRecycle(const std::wstring& payload, std::wstring& error,
                           pulse::shell::RecycleRestoreIndex& lookup, bool exact_destination = false) {
    const std::wstring wanted = CanonPath(payload);
    const std::wstring sid = pulse::CurrentUserSidString();
    if (wanted.size() < 3 || wanted[1] != L':' || sid.empty()) {
        error = L"无法确定当前用户的回收站";
        return false;
    }
    const std::wstring parent = wanted.substr(0, wanted.find_last_of(L'\\'));
    const std::wstring expected = CanonPath(wanted.substr(0, 2) + L"\\$Recycle.Bin\\" + sid);
    const std::wstring name = wanted.substr(wanted.find_last_of(L'\\') + 1);
    if (parent != expected || name.size() <= 2 || name.substr(0, 2) != L"$R") {
        // Legacy undo records contain only the original path. Never guess
        // between multiple versions of that path.
        const auto load = [](const std::wstring& root, pulse::shell::RecycleRestoreIndex::Entries& entries) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((root + L"\\$I*").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE) return;
            do {
                std::wstring original;
                const std::wstring index = root + L"\\" + data.cFileName;
                if (!ReadRecycleOriginal(index, original)) continue;
                std::wstring candidate = index;
                candidate[candidate.find_last_of(L'\\') + 2] = L'R';
                entries[CanonPath(original)].push_back(std::move(candidate));
            } while (FindNextFileW(find, &data));
            FindClose(find);
        };
        const std::wstring match = lookup.Resolve(expected, wanted, load, [&](const std::wstring& candidate) {
            if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
            std::wstring index = candidate, original;
            index[index.find_last_of(L'\\') + 2] = L'I';
            return ReadRecycleOriginal(index, original) && CanonPath(original) == wanted;
        });
        if (!match.empty()) return RestoreOneFromRecycle(match, error, lookup, exact_destination);
        error = L"无法唯一确定回收站版本，请在回收站中选择具体条目";
        return false;
    }
    return RestoreSelectedRecyclePayload(payload, error, exact_destination);
}

} // namespace pulse::recycle
