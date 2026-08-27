#include "diagnostics_exporter.h"
#include "pulse_version.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

namespace pulse::diagnostics {
namespace {

std::wstring JoinPath(std::wstring_view left, std::wstring_view right) {
    std::wstring value(left);
    if (!value.empty() && value.back() != L'\\') value.push_back(L'\\');
    value.append(right);
    return value;
}

void SetError(std::wstring* error, const wchar_t* value) {
    if (error) *error = value;
}

bool IsPlainDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool IsDirectoryEmpty(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool empty = true;
    do {
        if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return empty;
}

bool CopyNewFile(const std::wstring& source, const std::wstring& destination) {
    return CopyFileW(source.c_str(), destination.c_str(), TRUE) != FALSE;
}

bool IsCrashArtifact(std::wstring_view name, bool include_dumps) {
    if (name.ends_with(L".json") || name.ends_with(L".log")) return true;
    return include_dumps && name.ends_with(L".dmp");
}

bool CopyCrashArtifacts(const ExportOptions& options, size_t& copied) {
    const std::wstring source = JoinPath(options.source_root, L"Diagnostics\\Crashes");
    if (!IsPlainDirectory(source)) return true;
    const std::wstring destination = JoinPath(options.destination, L"Crashes");
    if (!CreateDirectoryW(destination.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(source, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring_view name(data.cFileName);
        if (!IsCrashArtifact(name, options.include_dumps)) continue;
        if (!CopyNewFile(JoinPath(source, name), JoinPath(destination, name))) {
            ok = false;
            break;
        }
        ++copied;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok;
}

bool WriteManifest(const ExportOptions& options, size_t copied) {
    const std::wstring path = JoinPath(options.destination, L"diagnostics-manifest.json");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char text[1024]{};
    const int length = snprintf(text, sizeof(text),
        "{\n  \"schema\":1,\n  \"version\":\"%ls\",\n  \"build_id\":\"%ls\",\n"
        "  \"utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\n"
        "  \"includes_dumps\":%s,\n  \"copied_files\":%zu\n}\n",
        PULSE_VERSION_STRING, PULSE_BUILD_ID, now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond,
        options.include_dumps ? "true" : "false", copied);
    DWORD written = 0;
    const BOOL ok = length > 0 && WriteFile(file, text, static_cast<DWORD>(length),
                                             &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == static_cast<DWORD>(length);
}

} // namespace

bool Export(const ExportOptions& options, std::wstring* error) {
    if (!IsPlainDirectory(options.source_root) || !IsPlainDirectory(options.destination)) {
        SetError(error, L"诊断源目录或目标目录不可用。");
        return false;
    }
    if (options.require_empty_destination && !IsDirectoryEmpty(options.destination)) {
        SetError(error, L"诊断导出目标必须为空目录。");
        return false;
    }

    size_t copied = 0;
    if (!CopyCrashArtifacts(options, copied)) {
        SetError(error, L"无法复制崩溃诊断文件。");
        return false;
    }
    static constexpr std::array<std::wstring_view, 4> logs = {
        L"pulse_crash.log", L"pulse_shell_host.log", L"material.log", L"index-service.log"
    };
    for (const auto name : logs) {
        const std::wstring source = JoinPath(options.source_root, name);
        if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!CopyNewFile(source, JoinPath(options.destination, name))) {
            SetError(error, L"无法复制诊断日志。");
            return false;
        }
        ++copied;
    }
    if (!WriteManifest(options, copied)) {
        SetError(error, L"无法写入诊断清单。");
        return false;
    }
    return true;
}

bool ClearCrashReports(const std::wstring& source_root, std::wstring* error) {
    const std::wstring source = JoinPath(source_root, L"Diagnostics\\Crashes");
    if (!IsPlainDirectory(source)) return true;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(source, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring_view name(data.cFileName);
        if (!IsCrashArtifact(name, true)) continue;
        if (!DeleteFileW(JoinPath(source, name).c_str())) ok = false;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (!ok) SetError(error, L"部分诊断文件无法删除。");
    return ok;
}

} // namespace pulse::diagnostics
