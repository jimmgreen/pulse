#include "transfer_snapshot.h"
#include <winternl.h>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <utility>
namespace pulse::extensions::transfer_snapshot {
Handles::~Handles() { Reset(); }
Handles::Handles(Handles&& other) noexcept : chain_(std::move(other.chain_)) { other.chain_.clear(); }
Handles& Handles::operator=(Handles&& other) noexcept {
    if (this != &other) { Reset(); chain_ = std::move(other.chain_); other.chain_.clear(); }
    return *this;
}
void Handles::Reset() { for (auto it = chain_.rbegin(); it != chain_.rend(); ++it) CloseHandle(*it); chain_.clear(); }
void Handles::Add(HANDLE handle) { chain_.push_back(handle); }
HANDLE Handles::Leaf() const { return chain_.empty() ? INVALID_HANDLE_VALUE : chain_.back(); }
HANDLE Handles::Parent() const { return chain_.size() < 2 ? INVALID_HANDLE_VALUE : chain_[chain_.size() - 2]; }
namespace {
bool Component(std::wstring_view value) {
    if (value.empty() || value == L"." || value == L".." || value.size() > 255 || value.back() == L'.' || value.back() == L' ') return false;
    for (wchar_t c : value) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
    std::wstring base(value.substr(0, value.find(L'.')));
    std::transform(base.begin(), base.end(), base.begin(), towupper);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" || base == L"CONOUT$") return false;
    if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
        ((base[3] >= L'0' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3')) return false;
    return true;
}
DWORD ReadIdentity(HANDLE handle, bool directory, transfer_plan::FileIdentity& identity) {
    if (GetFileType(handle) != FILE_TYPE_DISK) return ERROR_NOT_SUPPORTED;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) return GetLastError();
    if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_REPARSE_TAG_INVALID;
    if (!!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory || standard.DeletePending) return ERROR_FILE_INVALID;
    identity.volume = id.VolumeSerialNumber;
    std::memcpy(identity.file_id.data(), id.FileId.Identifier, identity.file_id.size());
    identity.size = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
    identity.last_write = static_cast<uint64_t>(basic.LastWriteTime.QuadPart);
    return ERROR_SUCCESS;
}
DWORD Open(const std::wstring& original, bool directory, const std::atomic<bool>& cancelled, Handles& handles, bool move = false, bool destination = false) {
    if (cancelled.load()) return ERROR_CANCELLED;
    std::wstring path = original;
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    if (path.size() < 3 || path.size() >= 32768 || !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' || path[2] != L'\\') return ERROR_INVALID_NAME;
    std::vector<std::wstring> parts;
    for (size_t from = 3; from < path.size();) {
        const auto end = path.find(L'\\', from);
        auto part = path.substr(from, end == std::wstring::npos ? path.size() - from : end - from);
        if (!Component(part)) return ERROR_INVALID_NAME;
        parts.push_back(std::move(part));
        if (end == std::wstring::npos) break;
        from = end + 1;
        if (from == path.size()) return ERROR_INVALID_NAME;
    }
    if (!directory && parts.empty()) return ERROR_INVALID_NAME;
    const auto root = path.substr(0, 3);
    const auto drive = GetDriveTypeW(root.c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) return ERROR_NOT_SUPPORTED;
    const std::wstring extended = L"\\\\?\\" + root;
    HANDLE handle = CreateFileW(extended.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    handles.Add(handle);
    transfer_plan::FileIdentity identity;
    if (const auto error = ReadIdentity(handle, true, identity)) return error;
    // Volume GUID names do not exist for network shares. Check the opened object,
    // not only the drive-letter mapping inspected before opening it.
    wchar_t final_path[32768]{};
    const DWORD count = GetFinalPathNameByHandleW(handle, final_path, 32768, VOLUME_NAME_GUID | FILE_NAME_OPENED);
    if (!count || count >= 32768 || !std::wstring_view(final_path, count).starts_with(L"\\\\?\\Volume{")) return ERROR_NOT_SUPPORTED;
    using OpenFile = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using StatusError = ULONG (WINAPI*)(NTSTATUS);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto open_file = reinterpret_cast<OpenFile>(GetProcAddress(module, "NtCreateFile"));
    const auto status_error = reinterpret_cast<StatusError>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!open_file || !status_error) return ERROR_NOT_SUPPORTED;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (cancelled.load()) return ERROR_CANCELLED;
        const bool is_directory = i + 1 < parts.size() || directory;
        UNICODE_STRING name{};
        name.Buffer = parts[i].data();
        name.Length = static_cast<USHORT>(parts[i].size() * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = handles.Leaf();
        attributes.ObjectName = &name;
        attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE, documented user-mode attribute.
        IO_STATUS_BLOCK status{};
        HANDLE next = INVALID_HANDLE_VALUE;
        // Single component relative opens never traverse an unchecked ancestor.
        const NTSTATUS result = open_file(&next, FILE_READ_ATTRIBUTES | SYNCHRONIZE |
            (is_directory ? FILE_TRAVERSE | (destination && i + 1 == parts.size() ? FILE_ADD_FILE : 0) : FILE_READ_DATA | (move ? DELETE : 0)), &attributes, &status, nullptr, 0,
            // Relative rename internally requires write sharing on its target directory.
            // Keep deny-delete and use this same object for every relative operation.
            FILE_SHARE_READ | (destination && i + 1 == parts.size() ? FILE_SHARE_WRITE : 0), 1 /* FILE_OPEN */, 0x00200000 /* FILE_OPEN_REPARSE_POINT */ |
            0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ | 0x00400000 /* FILE_OPEN_NO_RECALL */, nullptr, 0);
        if (result < 0) return status_error(result);
        handles.Add(next);
        if (const auto error = ReadIdentity(next, is_directory, identity)) return error;
    }
    return cancelled.load() ? ERROR_CANCELLED : ERROR_SUCCESS;
}
}
Result<Source> CaptureSource(const std::wstring& path, const std::atomic<bool>& cancelled) {
    Source source;
    if (const auto error = Open(path, false, cancelled, source.handles)) return {error, {}};
    source.snapshot.path = path;
    transfer_plan::FileIdentity parent;
    if (const auto error = ReadIdentity(source.handles.Leaf(), false, source.snapshot.identity)) return {error, {}};
    if (const auto error = ReadIdentity(source.handles.Parent(), true, parent)) return {error, {}};
    source.snapshot.parent = {parent.volume, parent.file_id};
    if (cancelled.load()) return {ERROR_CANCELLED, {}};
    return {ERROR_SUCCESS, std::move(source)};
}
Result<Target> CaptureTarget(const std::wstring& path, const std::atomic<bool>& cancelled) {
    Target target;
    if (const auto error = Open(path, true, cancelled, target.handles)) return {error, {}};
    target.snapshot.path = path;
    if (const auto error = ReadIdentity(target.handles.Leaf(), true, target.snapshot.identity)) return {error, {}};
    if (cancelled.load()) return {ERROR_CANCELLED, {}};
    return {ERROR_SUCCESS, std::move(target)};
}
Result<Source> ReopenSource(const transfer_plan::SourceFile& expected, const std::atomic<bool>& cancelled) {
    auto result = CaptureSource(expected.path, cancelled);
    if (result && (result.value->snapshot.identity != expected.identity || result.value->snapshot.parent != expected.parent))
        return {ERROR_FILE_INVALID, {}};
    return result;
}
Result<Source> AcquireTransferSource(const transfer_plan::SourceFile& expected, bool move, const std::atomic<bool>& cancelled) {
    Source source;
    if (const auto error = Open(expected.path, false, cancelled, source.handles, move)) return {error, {}};
    source.snapshot.path = expected.path;
    transfer_plan::FileIdentity parent;
    if (const auto error = ReadIdentity(source.handles.Leaf(), false, source.snapshot.identity)) return {error, {}};
    if (const auto error = ReadIdentity(source.handles.Parent(), true, parent)) return {error, {}};
    source.snapshot.parent = {parent.volume, parent.file_id};
    if (source.snapshot.identity != expected.identity || source.snapshot.parent != expected.parent) return {ERROR_FILE_INVALID, {}};
    if (cancelled.load()) return {ERROR_CANCELLED, {}};
    return {ERROR_SUCCESS, std::move(source)};
}
DWORD VerifyTransferTarget(HANDLE handle, const transfer_plan::TargetDirectory& expected) {
    transfer_plan::FileIdentity identity;
    if (const auto error = ReadIdentity(handle, true, identity)) return error;
    return identity.volume == expected.identity.volume && identity.file_id == expected.identity.file_id
        ? ERROR_SUCCESS : ERROR_FILE_INVALID;
}
Result<Target> AcquireTransferTarget(const transfer_plan::TargetDirectory& expected, const std::atomic<bool>& cancelled) {
    Target target;
    if (const auto error = Open(expected.path, true, cancelled, target.handles, false, true)) return {error, {}};
    target.snapshot.path = expected.path;
    if (const auto error = ReadIdentity(target.handles.Leaf(), true, target.snapshot.identity)) return {error, {}};
    if (target.snapshot.identity.volume != expected.identity.volume || target.snapshot.identity.file_id != expected.identity.file_id)
        return {ERROR_FILE_INVALID, {}};
    return {ERROR_SUCCESS, std::move(target)};
}
Result<Target> ReopenTarget(const transfer_plan::TargetDirectory& expected, const std::atomic<bool>& cancelled) {
    auto result = CaptureTarget(expected.path, cancelled);
    if (result && (result.value->snapshot.identity.volume != expected.identity.volume ||
        result.value->snapshot.identity.file_id != expected.identity.file_id)) return {ERROR_FILE_INVALID, {}};
    return result;
}
}
