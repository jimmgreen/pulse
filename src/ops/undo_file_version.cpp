#include "undo_file_version.h"
#include <bcrypt.h>
#include <winioctl.h>
#include <vector>

namespace pulse::ops {
namespace {
bool ReadVersion(HANDLE file, UndoFileVersion& out, DWORD& error) {
    BY_HANDLE_FILE_INFORMATION id{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(file, &id) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) {
        error = GetLastError(); return false;
    }
    if (id.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = ERROR_NOT_SUPPORTED; return false;
    }
    out = {};
    out.volume = id.dwVolumeSerialNumber; out.index_high = id.nFileIndexHigh; out.index_low = id.nFileIndexLow;
    out.attributes = id.dwFileAttributes;
    out.bytes = (static_cast<uint64_t>(id.nFileSizeHigh) << 32) | id.nFileSizeLow;
    out.modified = basic.LastWriteTime; out.changed = basic.ChangeTime;
    alignas(USN_RECORD_V2) unsigned char record[4096]{};
    DWORD returned = 0;
    if (DeviceIoControl(file, FSCTL_READ_FILE_USN_DATA, nullptr, 0, record, sizeof(record), &returned, nullptr) &&
        returned >= sizeof(USN_RECORD_V2)) {
        const auto* usn = reinterpret_cast<const USN_RECORD_V2*>(record);
        if (usn->MajorVersion == 2 && usn->RecordLength <= returned) { out.has_usn = true; out.usn = usn->Usn; }
    }
    error = ERROR_SUCCESS;
    return true;
}
bool SameMetadata(const UndoFileVersion& a, const UndoFileVersion& b) {
    return a.volume == b.volume && a.index_high == b.index_high && a.index_low == b.index_low &&
        a.bytes == b.bytes && a.attributes == b.attributes && a.modified.QuadPart == b.modified.QuadPart &&
        a.changed.QuadPart == b.changed.QuadPart &&
        (!a.has_usn || (b.has_usn && a.usn == b.usn));
}
bool Hash(HANDLE file, uint64_t limit, std::array<unsigned char, 32>& digest, DWORD& error) {
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup { BCRYPT_ALG_HANDLE& p; BCRYPT_HASH_HANDLE& h;
        ~Cleanup() { if (h) BCryptDestroyHash(h); if (p) BCryptCloseAlgorithmProvider(p, 0); }
    } cleanup{provider, hash};
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        error = ERROR_GEN_FAILURE; return false;
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) { error = GetLastError(); return false; }
    std::vector<unsigned char> buffer(64 * 1024);
    uint64_t consumed = 0;
    for (;;) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            error = GetLastError(); return false;
        }
        if (!bytes) break;
        if (bytes > limit - consumed) { error = ERROR_FILE_INVALID; return false; }
        consumed += bytes;
        if (BCryptHashData(hash, buffer.data(), bytes, 0) < 0) { error = ERROR_GEN_FAILURE; return false; }
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        error = ERROR_GEN_FAILURE; return false;
    }
    return true;
}
}
bool CaptureUndoFileVersion(const std::wstring& path, UndoFileVersion& out, DWORD& error, uint64_t hash_budget_bytes) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (hash_budget_bytes ? GENERIC_READ : 0),
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    struct Cleanup { HANDLE file; ~Cleanup() { CloseHandle(file); } } cleanup{file};
    if (!ReadVersion(file, out, error)) return false;
    if (hash_budget_bytes && out.bytes <= hash_budget_bytes) {
        if (!Hash(file, out.bytes, out.sha256, error)) return false;
        UndoFileVersion after;
        if (!ReadVersion(file, after, error)) return false;
        if (!SameMetadata(out, after)) { error = ERROR_FILE_INVALID; return false; }
        out.has_sha256 = true;
    }
    return true;
}
UndoFileLease::~UndoFileLease() { Reset(); }
void UndoFileLease::Reset() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}
bool UndoFileLease::Acquire(const std::wstring& path, const UndoFileVersion& expected, DWORD& error) {
    Reset();
    handle_ = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | (expected.has_sha256 ? GENERIC_READ : 0),
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    UndoFileVersion actual;
    bool ok = ReadVersion(handle_, actual, error);
    if (ok && !SameMetadata(expected, actual)) { ok = false; error = ERROR_FILE_INVALID; }
    if (ok && expected.has_sha256) {
        ok = Hash(handle_, expected.bytes, actual.sha256, error);
        if (ok && expected.sha256 != actual.sha256) { ok = false; error = ERROR_FILE_INVALID; }
    }
    if (!ok) Reset();
    return ok;
}
}
