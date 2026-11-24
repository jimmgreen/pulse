#pragma once
#include <windows.h>
#include <string>

namespace pulse::ops {
// Attributes of an open handle, reparse bit included. FAT32 and exFAT do not
// answer FileAttributeTagInfo (ERROR_INVALID_PARAMETER) and have no reparse
// points, so their basic attributes say the same. False: GetLastError says why.
inline bool QueryHandleAttributes(HANDLE handle, DWORD& attributes) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        attributes = tag.FileAttributes;
        return true;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION) return false;
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) return false;
    attributes = basic.FileAttributes;
    return true;
}

// One handle binds both metadata writes to the opened directory. Do not follow
// a replacement junction, or mutate an object that is no longer a directory.
inline bool ApplyDirectoryMetadata(const std::wstring& destination, const FILETIME& created,
                                   const FILETIME& accessed, const FILETIME& modified,
                                   DWORD attributes, DWORD& error) {
    error = ERROR_SUCCESS;
    HANDLE handle = CreateFileW(destination.c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    DWORD current = 0;
    bool ok = QueryHandleAttributes(handle, current);
    if (!ok) error = GetLastError();
    else if (!(current & FILE_ATTRIBUTE_DIRECTORY) || (current & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ok = false;
        error = ERROR_DIRECTORY;
    }
    if (ok && !SetFileTime(handle, &created, &accessed, &modified)) {
        ok = false;
        error = GetLastError();
    }
    if (ok) {
        FILE_BASIC_INFO basic{}; // Zero timestamps leave the times just written unchanged.
        basic.FileAttributes = attributes & ~(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY);
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
            ok = false;
            error = GetLastError();
        }
    }
    CloseHandle(handle);
    return ok;
}
}
