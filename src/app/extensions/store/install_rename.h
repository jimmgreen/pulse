#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::extensions::store::detail {
inline constexpr unsigned kMetadataRenameMaxAttempts = 12;
inline constexpr unsigned kPayloadRenameMaxAttempts = 36;

inline bool IsInstallRenameRetryable(DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    const std::u8string_view utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(utf8);
}

inline std::string TargetUtf8(const std::filesystem::path& target) {
    const auto utf8 = target.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

struct ScopedInstallHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ScopedInstallHandle() = default;
    ScopedInstallHandle(const ScopedInstallHandle&) = delete;
    ScopedInstallHandle& operator=(const ScopedInstallHandle&) = delete;
    ScopedInstallHandle(ScopedInstallHandle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    ScopedInstallHandle& operator=(ScopedInstallHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~ScopedInstallHandle() { Reset(); }
    void Reset() noexcept {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
        }
    }
};

struct FileIdentity {
    DWORD volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
    uint64_t size = 0;
    bool directory = false;
    bool valid = false;
};

inline bool QueryHandleIdentity(HANDLE handle, bool expected_directory, FileIdentity& out, DWORD& win32_error) noexcept {
    out = {};
    win32_error = 0;
    if (handle == INVALID_HANDLE_VALUE) {
        win32_error = ERROR_INVALID_HANDLE;
        return false;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        win32_error = GetLastError();
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    const bool is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_dir != expected_directory) {
        return false;
    }
    uint64_t size = 0;
    if (!expected_directory) {
        if (info.nNumberOfLinks != 1) {
            return false;
        }
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle, &length)) {
            win32_error = GetLastError();
            return false;
        }
        if (length.QuadPart < 0) {
            return false;
        }
        size = static_cast<uint64_t>(length.QuadPart);
    }
    out.volume = info.dwVolumeSerialNumber;
    out.index_high = info.nFileIndexHigh;
    out.index_low = info.nFileIndexLow;
    out.size = size;
    out.directory = is_dir;
    out.valid = true;
    return true;
}

inline bool MatchesIdentity(const FileIdentity& a, const FileIdentity& b) noexcept {
    return a.valid && b.valid && a.directory == b.directory && a.volume == b.volume &&
        a.index_high == b.index_high && a.index_low == b.index_low &&
        (a.directory || a.size == b.size);
}

inline void SleepCancellableSlice(DWORD delay_ms, const std::atomic<bool>& cancelled) {
    for (DWORD waited = 0; waited < delay_ms; waited += 25) {
        if (cancelled.load()) throw std::runtime_error("install_cancelled");
        Sleep((std::min)(DWORD{25}, delay_ms - waited));
    }
    if (cancelled.load()) throw std::runtime_error("install_cancelled");
}

inline DWORD BackoffDelayMs(unsigned attempt) noexcept {
    return (std::min)(DWORD{250}, DWORD{25} * (attempt + 1));
}

inline void WriteAllBytesAndFlush(HANDLE handle, std::string_view bytes, const std::atomic<bool>& cancelled) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancelled.load()) throw std::runtime_error("install_cancelled");
        DWORD count = 0;
        const auto wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        if (!WriteFile(handle, bytes.data() + offset, wanted, &count, nullptr) || count != wanted) {
            throw std::runtime_error("install_write:" + std::to_string(GetLastError()));
        }
        offset += count;
    }
    if (!FlushFileBuffers(handle)) {
        throw std::runtime_error("install_flush:" + std::to_string(GetLastError()));
    }
}

inline bool VerifyHandleBytes(HANDLE handle, std::string_view expected) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) {
        throw std::runtime_error("store_read:" + std::to_string(GetLastError()));
    }
    LARGE_INTEGER actual_size{};
    if (!GetFileSizeEx(handle, &actual_size)) {
        throw std::runtime_error("store_read:" + std::to_string(GetLastError()));
    }
    if (actual_size.QuadPart < 0 || static_cast<uint64_t>(actual_size.QuadPart) != expected.size()) {
        return false;
    }
    std::vector<char> buffer((std::min)(expected.size(), size_t{65536}));
    size_t offset = 0;
    while (offset < expected.size()) {
        DWORD count = 0;
        const auto wanted = static_cast<DWORD>((std::min)(expected.size() - offset, buffer.size()));
        if (!ReadFile(handle, buffer.data(), wanted, &count, nullptr) || count != wanted) {
            throw std::runtime_error("store_read:" + std::to_string(GetLastError()));
        }
        if (std::memcmp(buffer.data(), expected.data() + offset, wanted) != 0) {
            return false;
        }
        offset += wanted;
    }
    return true;
}

inline void BestEffortDeleteVerifiedFile(const std::filesystem::path& path, const FileIdentity& identity) noexcept {
    if (!identity.valid || identity.directory) return;
    for (int attempt = 0; attempt < 6; ++attempt) {
        ScopedInstallHandle handle;
        handle.value = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle.value == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return;
            if (!IsInstallRenameRetryable(code)) return;
            Sleep(15);
            continue;
        }
        FileIdentity current{};
        DWORD win32_error = 0;
        if (!QueryHandleIdentity(handle.value, false, current, win32_error) || !MatchesIdentity(current, identity)) {
            return;
        }
        FILE_DISPOSITION_INFO mark{TRUE};
        if (SetFileInformationByHandle(handle.value, FileDispositionInfo, &mark, sizeof(mark))) {
            return;
        }
        const DWORD code = GetLastError();
        handle.Reset();
        if (!IsInstallRenameRetryable(code)) return;
        Sleep(15);
    }
}

inline void BestEffortDeleteVerifiedDirectory(const std::filesystem::path& path, const FileIdentity& identity) noexcept {
    if (!identity.valid || !identity.directory) return;
    for (int attempt = 0; attempt < 6; ++attempt) {
        ScopedInstallHandle handle;
        handle.value = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle.value == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return;
            if (!IsInstallRenameRetryable(code)) return;
            Sleep(15);
            continue;
        }
        FileIdentity current{};
        DWORD win32_error = 0;
        if (!QueryHandleIdentity(handle.value, true, current, win32_error) || !MatchesIdentity(current, identity)) {
            return;
        }
        FILE_DISPOSITION_INFO mark{TRUE};
        if (SetFileInformationByHandle(handle.value, FileDispositionInfo, &mark, sizeof(mark))) {
            return;
        }
        const DWORD code = GetLastError();
        handle.Reset();
        if (RemoveDirectoryW(path.c_str())) return;
        const DWORD rm_code = GetLastError();
        if (!IsInstallRenameRetryable(code) && !IsInstallRenameRetryable(rm_code) && rm_code != ERROR_DIR_NOT_EMPTY) {
            return;
        }
        Sleep(15);
    }
}

struct StagedTempFile {
    std::filesystem::path path;
    FileIdentity identity;
    std::string expected_bytes;
    bool committed = false;

    StagedTempFile() = default;
    StagedTempFile(const StagedTempFile&) = delete;
    StagedTempFile& operator=(const StagedTempFile&) = delete;
    StagedTempFile(StagedTempFile&& other) noexcept
        : path(std::move(other.path)),
          identity(other.identity),
          expected_bytes(std::move(other.expected_bytes)),
          committed(std::exchange(other.committed, true)) {
        other.identity = {};
    }
    StagedTempFile& operator=(StagedTempFile&& other) noexcept {
        if (this != &other) {
            Cleanup();
            path = std::move(other.path);
            identity = other.identity;
            expected_bytes = std::move(other.expected_bytes);
            committed = std::exchange(other.committed, true);
            other.identity = {};
        }
        return *this;
    }
    ~StagedTempFile() { Cleanup(); }

private:
    void Cleanup() noexcept {
        if (!committed && identity.valid) {
            BestEffortDeleteVerifiedFile(path, identity);
        }
    }
};

inline StagedTempFile WriteStagedTempFile(const std::filesystem::path& parent, std::wstring_view prefix,
    std::string_view bytes, const std::atomic<bool>& cancelled) {
    static std::atomic<uint64_t> serial{0};
    StagedTempFile staged;
    ScopedInstallHandle handle;
    for (int attempt = 0; attempt < 32; ++attempt) {
        if (cancelled.load()) throw std::runtime_error("install_cancelled");
        staged.path = parent / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        handle.value = CreateFileW(staged.path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle.value != INVALID_HANDLE_VALUE) break;
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("install_create_temporary:" + std::to_string(code));
        }
    }
    if (handle.value == INVALID_HANDLE_VALUE) throw std::runtime_error("install_temporary_collision");

    FileIdentity created_identity{};
    DWORD id_error = 0;
    if (!QueryHandleIdentity(handle.value, false, created_identity, id_error)) {
        if (id_error != 0) throw std::runtime_error("store_file_info:" + std::to_string(id_error));
        throw std::runtime_error("store_requires_ordinary_path");
    }
    try {
        WriteAllBytesAndFlush(handle.value, bytes, cancelled);
    } catch (...) {
        FILE_DISPOSITION_INFO mark{TRUE};
        // Handle was created exclusively by us (CREATE_NEW, share=READ), so mark-on-close works before Reset().
        (void)SetFileInformationByHandle(handle.value, FileDispositionInfo, &mark, sizeof(mark));
        handle.Reset();
        throw;
    }
    created_identity.size = bytes.size();
    staged.identity = created_identity;
    staged.expected_bytes.assign(bytes);
    // Close write handle immediately so IRP_MJ_CLEANUP runs and security scanners can scan cleanly.
    handle.Reset();
    return staged;
}

inline std::vector<unsigned char> BuildFileRenameInfoStorage(const std::filesystem::path& target, bool replace) {
    const auto name = target.native();
    const size_t bytes = name.size() * sizeof(wchar_t);
    if (bytes > 65500) throw std::runtime_error("install_path_limit");
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = replace ? TRUE : FALSE;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, name.data(), bytes);
    return storage;
}

inline void RenameStagedTempFile(StagedTempFile& staged, const std::filesystem::path& target, bool replace,
    const std::atomic<bool>& cancelled, unsigned max_attempts = kMetadataRenameMaxAttempts,
    const std::function<void(const std::filesystem::path&, DWORD)>& on_retry = {}) {
    auto storage = BuildFileRenameInfoStorage(target, replace);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    const DWORD storage_size = static_cast<DWORD>(storage.size());
    for (unsigned attempt = 0;; ++attempt) {
        if (cancelled.load()) throw std::runtime_error("install_cancelled");
        ScopedInstallHandle handle;
        handle.value = CreateFileW(staged.path.c_str(), GENERIC_READ | DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle.value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (!IsInstallRenameRetryable(error) || attempt == max_attempts) {
                throw std::runtime_error("install_atomic_rename:" + std::to_string(error) + ":target=" +
                    TargetUtf8(target));
            }
            if (on_retry) on_retry(target, error);
            SleepCancellableSlice(BackoffDelayMs(attempt), cancelled);
            continue;
        }
        FileIdentity current{};
        DWORD id_error = 0;
        if (!QueryHandleIdentity(handle.value, false, current, id_error)) {
            if (id_error != 0) throw std::runtime_error("store_file_info:" + std::to_string(id_error));
            throw std::runtime_error("store_requires_ordinary_path");
        }
        if (!MatchesIdentity(current, staged.identity) || !VerifyHandleBytes(handle.value, staged.expected_bytes)) {
            throw std::runtime_error("immutable_revision_conflict");
        }
        if (SetFileInformationByHandle(handle.value, FileRenameInfo, info, storage_size)) {
            staged.committed = true;
            return;
        }
        const DWORD error = GetLastError();
        // Close handle immediately before sleeping so scanners never stall waiting on our open handle.
        handle.Reset();
        if (!IsInstallRenameRetryable(error) || attempt == max_attempts) {
            throw std::runtime_error("install_atomic_rename:" + std::to_string(error) + ":target=" +
                TargetUtf8(target));
        }
        if (on_retry) on_retry(target, error);
        SleepCancellableSlice(BackoffDelayMs(attempt), cancelled);
    }
}

struct StagedMemberRecord {
    std::filesystem::path path;
    FileIdentity identity;
};

struct StagedPayloadDir {
    std::filesystem::path path;
    FileIdentity identity;
    std::vector<StagedMemberRecord> members;
    bool committed = false;

    StagedPayloadDir() = default;
    StagedPayloadDir(const StagedPayloadDir&) = delete;
    StagedPayloadDir& operator=(const StagedPayloadDir&) = delete;
    StagedPayloadDir(StagedPayloadDir&& other) noexcept
        : path(std::move(other.path)),
          identity(other.identity),
          members(std::move(other.members)),
          committed(std::exchange(other.committed, true)) {
        other.identity = {};
    }
    StagedPayloadDir& operator=(StagedPayloadDir&& other) noexcept {
        if (this != &other) {
            Cleanup();
            path = std::move(other.path);
            identity = other.identity;
            members = std::move(other.members);
            committed = std::exchange(other.committed, true);
            other.identity = {};
        }
        return *this;
    }
    ~StagedPayloadDir() { Cleanup(); }

private:
    void Cleanup() noexcept {
        if (!committed && identity.valid) {
            for (const auto& member : members) {
                BestEffortDeleteVerifiedFile(member.path, member.identity);
            }
            BestEffortDeleteVerifiedDirectory(path, identity);
        }
    }
};

inline bool IsValidMemberFilename(std::string_view member_name, std::filesystem::path& relative_out) {
    relative_out.clear();
    if (member_name.empty() || member_name == "." || member_name == "..") return false;
    for (char c : member_name) {
        if (c == '\0' || c == '/' || c == '\\' || c == ':') return false;
    }
    if (member_name.back() == '.' || member_name.back() == ' ') return false;
    auto relative = PathFromUtf8(member_name);
    const auto& native = relative.native();
    if (native.empty() || native == L"." || native == L".." || native.back() == L'.' || native.back() == L' ') {
        return false;
    }
    if (relative.has_parent_path() || relative.has_root_path() || relative.filename() != relative) {
        return false;
    }
    relative_out = std::move(relative);
    return true;
}

inline StagedPayloadDir CreateStagedPayloadDir(const std::filesystem::path& parent, std::wstring_view prefix,
    const std::atomic<bool>& cancelled) {
    static std::atomic<uint64_t> serial{0};
    StagedPayloadDir dir;
    for (int attempt = 0; attempt < 32; ++attempt) {
        if (cancelled.load()) throw std::runtime_error("install_cancelled");
        dir.path = parent / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        if (CreateDirectoryW(dir.path.c_str(), nullptr)) {
            ScopedInstallHandle probe;
            probe.value = CreateFileW(dir.path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (probe.value == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("store_open:" + std::to_string(GetLastError()));
            }
            DWORD id_error = 0;
            if (!QueryHandleIdentity(probe.value, true, dir.identity, id_error)) {
                if (id_error != 0) throw std::runtime_error("store_file_info:" + std::to_string(id_error));
                throw std::runtime_error("store_requires_ordinary_path");
            }
            return dir;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("install_payload_directory:" + std::to_string(code));
        }
    }
    throw std::runtime_error("install_payload_collision");
}

inline void WriteStagedMemberFile(StagedPayloadDir& dir, std::string_view member_name, std::string_view bytes,
    const std::atomic<bool>& cancelled) {
    std::filesystem::path relative;
    if (!IsValidMemberFilename(member_name, relative)) {
        throw std::runtime_error("store_requires_ordinary_path");
    }
    const auto member_path = dir.path / relative;
    ScopedInstallHandle handle;
    handle.value = CreateFileW(member_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("install_create_temporary:" + std::to_string(GetLastError()));
    }
    FileIdentity member_id{};
    DWORD id_error = 0;
    if (!QueryHandleIdentity(handle.value, false, member_id, id_error)) {
        if (id_error != 0) throw std::runtime_error("store_file_info:" + std::to_string(id_error));
        throw std::runtime_error("store_requires_ordinary_path");
    }
    try {
        WriteAllBytesAndFlush(handle.value, bytes, cancelled);
    } catch (...) {
        FILE_DISPOSITION_INFO mark{TRUE};
        (void)SetFileInformationByHandle(handle.value, FileDispositionInfo, &mark, sizeof(mark));
        handle.Reset();
        throw;
    }
    member_id.size = bytes.size();
    handle.Reset();
    dir.members.push_back(StagedMemberRecord{member_path, member_id});
}

inline void RenameStagedDirectory(StagedPayloadDir& dir, const std::filesystem::path& target,
    const std::atomic<bool>& cancelled, unsigned max_attempts = kPayloadRenameMaxAttempts,
    const std::function<void(const std::filesystem::path&, DWORD)>& on_retry = {}) {
    auto storage = BuildFileRenameInfoStorage(target, false);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    const DWORD storage_size = static_cast<DWORD>(storage.size());
    for (unsigned attempt = 0;; ++attempt) {
        if (cancelled.load()) throw std::runtime_error("install_cancelled");
        ScopedInstallHandle handle;
        handle.value = CreateFileW(dir.path.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle.value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (!IsInstallRenameRetryable(error) || attempt == max_attempts) {
                throw std::runtime_error("install_atomic_rename:" + std::to_string(error) + ":target=" +
                    TargetUtf8(target));
            }
            if (on_retry) on_retry(target, error);
            SleepCancellableSlice(BackoffDelayMs(attempt), cancelled);
            continue;
        }
        FileIdentity current{};
        DWORD id_error = 0;
        if (!QueryHandleIdentity(handle.value, true, current, id_error)) {
            if (id_error != 0) throw std::runtime_error("store_file_info:" + std::to_string(id_error));
            throw std::runtime_error("store_requires_ordinary_path");
        }
        if (!MatchesIdentity(current, dir.identity)) {
            throw std::runtime_error("immutable_revision_conflict");
        }
        if (SetFileInformationByHandle(handle.value, FileRenameInfo, info, storage_size)) {
            dir.committed = true;
            return;
        }
        const DWORD error = GetLastError();
        // Close handle immediately before sleeping so scanners on descendant files can finish and close cleanly.
        handle.Reset();
        if (!IsInstallRenameRetryable(error) || attempt == max_attempts) {
            throw std::runtime_error("install_atomic_rename:" + std::to_string(error) + ":target=" +
                TargetUtf8(target));
        }
        if (on_retry) on_retry(target, error);
        SleepCancellableSlice(BackoffDelayMs(attempt), cancelled);
    }
}
} // namespace pulse::extensions::store::detail
