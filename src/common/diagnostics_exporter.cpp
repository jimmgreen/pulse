#include "diagnostics_exporter.h"
#include "diagnostics_cleanup_io.h"
#include "diagnostics_report.h"
#include "runtime_log.h"
#include "pulse_version.h"
#include "localization.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <string_view>
#include <utility>
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
    if (empty && GetLastError() != ERROR_NO_MORE_FILES) empty = false;
    FindClose(find);
    return empty;
}

constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxFiles = 256;

struct FileRecord {
    std::wstring name;
    uint64_t bytes = 0;
    DWORD error = ERROR_SUCCESS;
    bool missing = false;
    uint64_t rejected_lines = 0;
};

struct ExportState {
    std::vector<FileRecord> records;
    uint64_t bytes = 0;
    size_t copied = 0;
    bool ok = true;
};

bool IsMissing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool IsCrashArtifact(std::wstring_view name, bool include_dumps) {
    if (name.ends_with(L".json") || name.ends_with(L".log")) return true;
    return include_dumps && name.ends_with(L".dmp");
}

bool IsRuntimeArtifact(std::wstring_view name) {
    if (name.ends_with(L".1")) name.remove_suffix(2);
    if (!name.ends_with(L".jsonl")) return false;
    name.remove_suffix(6);
    if (name.ends_with(L".critical")) name.remove_suffix(9);
    static constexpr std::array<std::wstring_view, 10> prefixes = {
        L"app-", L"index-", L"index-service-", L"index-helper-", L"network-agent-",
        L"content-agent-", L"preview-", L"shell-", L"test-", L"unknown-"
    };
    const auto prefix = std::find_if(prefixes.begin(), prefixes.end(), [name](auto value) {
        return name.starts_with(value) && name.size() > value.size() &&
            name[value.size()] >= L'0' && name[value.size()] <= L'9';
    });
    if (prefix == prefixes.end()) return false;
    name.remove_prefix(prefix->size());
    return !name.empty() && std::all_of(name.begin(), name.end(),
        [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

// Keep source/destination ancestors open without delete sharing while exporting.
// This both rejects junctions and prevents a checked directory being replaced.
struct DirectoryLocks {
    std::vector<HANDLE> handles;
    ~DirectoryLocks() { for (HANDLE handle : handles) CloseHandle(handle); }

    DWORD Lock(const std::wstring& path) {
        const DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (!length) return GetLastError();
        std::wstring absolute(length, L'\0');
        const DWORD written = GetFullPathNameW(path.c_str(), length, absolute.data(), nullptr);
        if (!written || written >= length) return ERROR_INVALID_NAME;
        absolute.resize(written);
        size_t root_end = absolute.starts_with(L"\\\\?\\") ? 6 : 2;
        const size_t server_start = absolute.starts_with(L"\\\\?\\UNC\\") ? 8 :
            absolute.starts_with(L"\\\\") && !absolute.starts_with(L"\\\\?\\") ? 2 : 0;
        if (server_start) {
            const size_t server_end = absolute.find(L'\\', server_start);
            const size_t share_end = server_end == std::wstring::npos ? server_end :
                absolute.find(L'\\', server_end + 1);
            root_end = share_end == std::wstring::npos ? absolute.size() : share_end;
        }
        while (absolute.size() > root_end + 1 && absolute.back() == L'\\') absolute.pop_back();
        std::vector<std::wstring> parents;
        for (std::wstring current = absolute;;) {
            parents.push_back(current);
            const auto slash = current.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash <= root_end) break;
            current.resize(slash);
        }
        for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
            HANDLE handle = CreateFileW(it->c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return GetLastError();
            BY_HANDLE_FILE_INFORMATION info{};
            const BOOL queried = GetFileInformationByHandle(handle, &info);
            DWORD error = queried ? ERROR_SUCCESS : GetLastError();
            if (queried && (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))
                error = ERROR_ACCESS_DENIED;
            if (error) { CloseHandle(handle); return error; }
            handles.push_back(handle);
        }
        return ERROR_SUCCESS;
    }
};

void RecordError(ExportState& state, std::wstring name, DWORD error, bool optional = false) {
    const bool missing = optional && IsMissing(error);
    state.records.push_back({std::move(name), 0, error, missing});
    if (!missing) state.ok = false;
}

void CopyArtifact(const std::wstring& source, const std::wstring& destination,
                  std::wstring name, ExportState& state, bool optional = false, bool sanitize = false) {
    FileRecord record{std::move(name)};
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        RecordError(state, std::move(record.name), GetLastError(), optional);
        return;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(input, &info) || !GetFileSizeEx(input, &size))
        record.error = GetLastError();
    else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        record.error = ERROR_ACCESS_DENIED;
    else if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes ||
             static_cast<uint64_t>(size.QuadPart) > kMaxTotalBytes - state.bytes)
        record.error = ERROR_FILE_TOO_LARGE;
    HANDLE output = INVALID_HANDLE_VALUE;
    if (!record.error) {
        output = CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) record.error = GetLastError();
    }
    std::array<char, 64 * 1024> buffer{};
    std::string runtime_text;
    uint64_t consumed = 0;
    // Snapshot the initial length, so an active writer cannot grow the export forever.
    while (!record.error && consumed < static_cast<uint64_t>(size.QuadPart)) {
        const DWORD wanted = static_cast<DWORD>((std::min)(
            static_cast<uint64_t>(buffer.size()), static_cast<uint64_t>(size.QuadPart) - consumed));
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), wanted, &read, nullptr)) {
            record.error = GetLastError();
            break;
        }
        if (!read) { record.error = ERROR_HANDLE_EOF; break; }
        consumed += read;
        if (sanitize) { runtime_text.append(reinterpret_cast<const char*>(buffer.data()), read); continue; }
        DWORD written = 0;
        const BOOL wrote = WriteFile(output, buffer.data(), read, &written, nullptr);
        record.bytes += written;
        if (!wrote || written != read) record.error = wrote ? ERROR_WRITE_FAULT : GetLastError();
    }
    if (sanitize && !record.error) {
        const auto safe = SanitizeRuntime(runtime_text, record.rejected_lines);
        DWORD written = 0;
        if (!WriteFile(output, safe.data(), static_cast<DWORD>(safe.size()), &written, nullptr) || written != safe.size())
            record.error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        record.bytes = written;
    }
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    CloseHandle(input);
    state.bytes += record.bytes;
    if (record.error) state.ok = false;
    else ++state.copied;
    state.records.push_back(std::move(record));
}

void CopyDirectoryArtifacts(const ExportOptions& options, std::wstring_view folder,
                            bool runtime, ExportState& state, DirectoryLocks& locks) {
    const std::wstring source = JoinPath(JoinPath(options.source_root, L"Diagnostics"), folder);
    const DWORD source_error = locks.Lock(source);
    if (source_error) {
        RecordError(state, std::wstring(folder) + L"/", source_error, true);
        return;
    }
    const std::wstring destination = JoinPath(options.destination, folder);
    if (!CreateDirectoryW(destination.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        RecordError(state, std::wstring(folder) + L"/", GetLastError());
        return;
    }
    const DWORD destination_error = locks.Lock(destination);
    if (destination_error) {
        RecordError(state, std::wstring(folder) + L"/", destination_error);
        return;
    }
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(source, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) RecordError(state, std::wstring(folder) + L"/", error);
        return;
    }
    do {
        const std::wstring_view name(data.cFileName);
        if (!(runtime ? IsRuntimeArtifact(name) : IsCrashArtifact(name, options.include_dumps)))
            continue;
        if (state.records.size() >= kMaxFiles) {
            RecordError(state, std::wstring(folder) + L"/", ERROR_TOO_MANY_OPEN_FILES);
            break;
        }
        CopyArtifact(JoinPath(source, name), JoinPath(destination, name),
            std::wstring(folder) + L"/" + std::wstring(name), state, false, runtime && options.support_report);
    } while (FindNextFileW(find, &data));
    const DWORD enumeration_error = GetLastError();
    FindClose(find);
    if (enumeration_error != ERROR_NO_MORE_FILES && state.records.size() <= kMaxFiles)
        RecordError(state, std::wstring(folder) + L"/", enumeration_error);
}

std::string JsonString(std::wstring_view value) {
    std::string text = "\"";
    for (wchar_t ch : value) {
        if (ch >= 32 && ch < 127 && ch != L'"' && ch != L'\\')
            text.push_back(static_cast<char>(ch));
        else {
            char escaped[7]{};
            snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(ch));
            text += escaped;
        }
    }
    text += '"';
    return text;
}

bool WriteManifest(const ExportOptions& options, const ExportState& state) {
    const std::wstring path = JoinPath(options.destination, L"diagnostics-manifest.json");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char timestamp[32]{};
    snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::string text = "{\n  \"schema\":2,\n  \"version\":" + JsonString(PULSE_VERSION_STRING) +
        ",\n  \"build_id\":" + JsonString(PULSE_BUILD_ID) + ",\n  \"utc\":\"" + timestamp +
        "\",\n  \"includes_dumps\":" + (options.include_dumps ? "true" : "false") +
        ",\n  \"complete\":" + (state.ok ? "true" : "false") +
        ",\n  \"copied_files\":" + std::to_string(state.copied) +
        ",\n  \"copied_bytes\":" + std::to_string(state.bytes) +
        ",\n  \"max_files\":" + std::to_string(kMaxFiles) +
        ",\n  \"max_file_bytes\":" + std::to_string(kMaxFileBytes) +
        ",\n  \"max_total_bytes\":" + std::to_string(kMaxTotalBytes) + ",\n  \"files\":[";
    bool first = true;
    for (const auto& record : state.records) {
        if (!first) text += ',';
        first = false;
        text += "\n    {\"name\":" + JsonString(record.name) +
            ",\"bytes\":" + std::to_string(record.bytes) +
            ",\"error_code\":" + std::to_string(record.error) +
            ",\"rejected_lines\":" + std::to_string(record.rejected_lines) +
            ",\"status\":\"" + (record.missing ? "missing" : record.error ? "error" : "copied") + "\"}";
    }
    text += "\n  ]\n}\n";
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == text.size();
}

} // namespace

bool Export(const ExportOptions& options, std::wstring* error) {
    if (error) error->clear();
    const bool flushed = options.support_report && runtime::Flush(2000);
    const auto logger_health = runtime::GetHealth();
    DirectoryLocks locks;
    if (locks.Lock(options.source_root) || locks.Lock(options.destination)) {
        SetError(error, l10n::Pick(L"诊断源目录或目标目录不可用，或包含重解析点。",
            L"The diagnostics source or destination folder is unavailable or contains a reparse point."));
        return false;
    }
    if (options.require_empty_destination && !IsDirectoryEmpty(options.destination)) {
        SetError(error, l10n::Pick(L"诊断导出目标必须为空目录。", L"The diagnostics export destination must be an empty folder."));
        return false;
    }
    ExportState state;
    static constexpr std::array<std::wstring_view, 5> logs = {
        L"pulse_crash.log", L"pulse_shell_host.log", L"material.log", L"index-service.log", L"pulse_graphics.log"
    };
    if (!options.support_report) for (const auto name : logs) {
        CopyArtifact(JoinPath(options.source_root, name), JoinPath(options.destination, name),
            std::wstring(name), state, true);
    }
    CopyDirectoryArtifacts(options, L"Runtime", true, state, locks);
    if (!options.support_report || options.include_dumps)
        CopyDirectoryArtifacts(options, L"Crashes", false, state, locks);
    if (!WriteManifest(options, state)) {
        SetError(error, l10n::Pick(L"无法写入诊断清单。", L"Could not write the diagnostics manifest."));
        return false;
    }
    if (options.support_report && !WriteSupportReport(options, state.ok, flushed, logger_health, error)) return false;
    if (!state.ok) {
        SetError(error, l10n::Pick(L"部分诊断文件未能导出；详情见目标目录中的 diagnostics-manifest.json。",
            L"Some diagnostics files could not be exported; see diagnostics-manifest.json in the destination."));
    }
    return state.ok;
}
bool ClearCrashReports(const std::wstring& source_root, std::wstring* error) {
    return ClearCrashReportsWithIo(source_root, error, CleanupIo{});
}
bool ClearCrashReportsWithIo(const std::wstring& source_root, std::wstring* error, const CleanupIo& io) {
    if (error) error->clear();
    const auto fail = [error](DWORD code) {
        SetError(error, l10n::Pick(L"部分诊断文件无法删除。", L"Some diagnostics files could not be deleted."));
        if (error) *error += L" (" + std::to_wstring(code) + L")";
        return false;
    };
    const std::wstring source = JoinPath(source_root, L"Diagnostics\\Crashes");
    const DWORD attributes = io.attributes(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        return IsMissing(code) ? true : fail(code);
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(ERROR_ACCESS_DENIED);
    DirectoryLocks locks;
    const DWORD lock_error = locks.Lock(source);
    if (lock_error != ERROR_SUCCESS) return IsMissing(lock_error) ? true : fail(lock_error);
    WIN32_FIND_DATAW data{};
    HANDLE find = io.first(JoinPath(source, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_NO_MORE_FILES ? true : fail(code);
    }
    DWORD failure = ERROR_SUCCESS;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring_view name(data.cFileName);
        if (!IsCrashArtifact(name, true)) continue;
        if (!DeleteFileW(JoinPath(source, name).c_str())) {
            const DWORD code = GetLastError();
            if (!IsMissing(code) && failure == ERROR_SUCCESS) failure = code;
        }
    } while (io.next(find, &data));
    const DWORD enumeration_error = GetLastError();
    FindClose(find);
    if (enumeration_error != ERROR_NO_MORE_FILES) failure = enumeration_error;
    return failure == ERROR_SUCCESS ? true : fail(failure);
}

} // namespace pulse::diagnostics
