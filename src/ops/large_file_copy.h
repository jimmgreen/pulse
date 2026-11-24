// large_file_copy.h — Unbuffered big-chunk copy of one large file between local disks.
//
// CopyFile2 with COPY_FILE_NO_BUFFERING moves about 1 MiB per request. When
// source and target share a spinning disk, every request makes the head travel
// between the two files, so a same-disk copy runs at well under half the disk's
// sequential speed. Here a chunk of tens of MiB is read, then written: the head
// travels once per chunk. Between different disks a read and a write are kept
// in flight together.
//
// The copy matches what CopyFile2 leaves: the default stream, named streams,
// attributes and the last-write time; creation time is the copy's own, the
// security descriptor is the target folder's. Files CopyFile2 has to handle
// specially (encrypted, compressed, sparse, links, extended attributes) are not
// taken: S_FALSE, nothing created, the caller uses CopyFile2.
#pragma once
#include <windows.h>
#include <winioctl.h>
#include "owned_transfer_output.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ops {

struct LargeCopyControl {
    std::atomic<bool>* cancel = nullptr;
    std::atomic<bool>* skip = nullptr;
    std::atomic<bool>* pause = nullptr;
    // Blocks while paused; false: cancelled or skipped while waiting.
    std::function<bool()> wait_while_paused;
    std::function<void(uint64_t done, uint64_t total)> report;
    // Between chunks, with nothing in flight: lets the caller hand a shared
    // disk to another operation and wait for it back. False: abort.
    std::function<bool()> checkpoint;
};

struct LargeCopyPlan {
    size_t chunk = 64u << 20;  // bytes per request, a multiple of 1 MiB
    bool overlap = false;      // a read and a write in flight together (different disks)
};

namespace large_copy_detail {

inline std::wstring VolumeDevice(const std::wstring& path) {
    std::vector<wchar_t> root(32768);
    if (!GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.data(), volume, MAX_PATH)) return {};
    std::wstring device = volume;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    return device;
}

// Physical disk of a volume, or -1 (several disks, or not known).
inline long DiskNumber(const std::wstring& path) {
    const std::wstring device = VolumeDevice(path);
    if (device.empty()) return -1;
    HANDLE volume = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return -1;
    STORAGE_DEVICE_NUMBER number{};
    DWORD returned = 0;
    const bool ok = DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number),
                                    &returned, nullptr) != FALSE;
    CloseHandle(volume);
    return ok ? static_cast<long>(number.DeviceNumber) : -1;
}

inline bool HasExtendedAttributes(HANDLE file) {
    using QueryInformationFile = LONG(NTAPI*)(HANDLE, void*, void*, ULONG, ULONG);
    static const auto query = reinterpret_cast<QueryInformationFile>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile"));
    if (!query) return true;  // cannot tell: leave it to CopyFile2
    struct { LONG_PTR status; ULONG_PTR information; } io{};
    ULONG ea_size = 0;
    constexpr ULONG kFileEaInformation = 7;
    if (query(file, &io, &ea_size, sizeof(ea_size), kFileEaInformation) < 0) return true;
    return ea_size != 0;
}

struct NamedStream { std::wstring name; uint64_t bytes; };

// Named $DATA streams (":Zone.Identifier:$DATA"), false when they cannot be listed.
inline bool NamedStreams(const std::wstring& path, std::vector<NamedStream>& streams) {
    WIN32_FIND_STREAM_DATA data{};
    HANDLE find = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &data, 0);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_HANDLE_EOF;
    do {
        const std::wstring name = data.cStreamName;
        if (name != L"::$DATA") streams.push_back({name, static_cast<uint64_t>(data.StreamSize.QuadPart)});
    } while (FindNextStreamW(find, &data));
    const DWORD error = GetLastError();
    FindClose(find);
    return error == ERROR_HANDLE_EOF;
}

inline DWORD CopyNamedStream(const std::wstring& source, const std::wstring& destination, const NamedStream& stream) {
    std::wstring name = stream.name;
    if (const size_t type = name.rfind(L":$DATA"); type != std::wstring::npos && type > 0) name.resize(type);
    HANDLE in = CreateFileW((source + name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return GetLastError();
    // The copy's main handle holds DELETE access, which every stream of the file must share.
    HANDLE out = CreateFileW((destination + name).c_str(), GENERIC_WRITE, FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) { const DWORD error = GetLastError(); CloseHandle(in); return error; }
    std::vector<char> buffer(1u << 20);
    DWORD error = ERROR_SUCCESS;
    for (;;) {
        DWORD read = 0, written = 0;
        if (!ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { error = GetLastError(); break; }
        if (read == 0) break;
        if (!WriteFile(out, buffer.data(), read, &written, nullptr) || written != read) {
            error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            break;
        }
    }
    CloseHandle(out);
    CloseHandle(in);
    return error;
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Buffer {
    void* data = nullptr;
    explicit Buffer(size_t bytes) { data = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
    ~Buffer() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
};
struct Request {
    OVERLAPPED overlapped{};
    HANDLE file = INVALID_HANDLE_VALUE;
    bool pending = false;
    Request() { overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~Request() {
        // Leaving early with a request in flight: it must not land in a freed buffer.
        if (pending) {
            CancelIoEx(file, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(file, &overlapped, &ignored, TRUE);
        }
        if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
    }
    bool Start(HANDLE target, uint64_t offset, bool write, void* data, DWORD bytes, DWORD& error) {
        file = target;
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ResetEvent(overlapped.hEvent);
        const BOOL done = write ? WriteFile(file, data, bytes, nullptr, &overlapped)
                                : ReadFile(file, data, bytes, nullptr, &overlapped);
        if (!done && GetLastError() != ERROR_IO_PENDING) {
            error = GetLastError();
            return false;
        }
        pending = true;
        return true;
    }
    bool Finish(HANDLE target, DWORD& transferred, DWORD& error) {
        pending = false;
        if (GetOverlappedResult(target, &overlapped, &transferred, TRUE)) return true;
        error = GetLastError();
        if (error == ERROR_HANDLE_EOF) { transferred = 0; error = ERROR_SUCCESS; return true; }
        return false;
    }
};

}  // namespace large_copy_detail

// Same physical disk: the big-chunk read-then-write order matters most there.
inline bool SamePhysicalDisk(const std::wstring& source, const std::wstring& destination_dir) {
    const long a = large_copy_detail::DiskNumber(source);
    return a >= 0 && a == large_copy_detail::DiskNumber(destination_dir);
}

// S_OK; S_FALSE when the file is left to CopyFile2 (nothing created);
// HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) when cancelled or skipped; or the
// failure. `owned_output` gets the destination's identity once it is created.
// The destination is removed on any failure that created it.
inline HRESULT CopyLargeFile(const std::wstring& source, const std::wstring& destination, bool fail_if_exists,
                             const LargeCopyPlan& plan, const LargeCopyControl& control,
                             std::shared_ptr<VerificationStamp>* owned_output = nullptr) {
    using namespace large_copy_detail;
    constexpr DWORD kUnhandled = FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE |
        FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY;
    Handle in;
    in.value = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(in.value, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(in.value, FileStandardInfo, &standard, sizeof(standard)))
        return HRESULT_FROM_WIN32(GetLastError());
    // FAT32 and exFAT have neither named streams nor extended attributes; asking
    // them fails, which must not read as "cannot tell".
    DWORD source_volume = 0;
    if (!GetVolumeInformationByHandleW(in.value, nullptr, 0, nullptr, nullptr, &source_volume, nullptr, 0))
        return S_FALSE;
    if ((basic.FileAttributes & kUnhandled) ||
        ((source_volume & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) && HasExtendedAttributes(in.value)))
        return S_FALSE;
    std::vector<NamedStream> streams;
    if ((source_volume & FILE_NAMED_STREAMS) && !NamedStreams(source, streams)) return S_FALSE;
    const uint64_t total = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
    // Whole MiBs, never more than the file needs.
    constexpr uint64_t kMiB = uint64_t{1} << 20;
    const uint64_t file_mib = (std::max<uint64_t>)((total + kMiB - 1) / kMiB, 1);
    const size_t chunk = static_cast<size_t>((std::min<uint64_t>)((std::max<uint64_t>)(plan.chunk / kMiB, 1), file_mib) * kMiB);
    Buffer first(chunk), second(plan.overlap ? chunk : 0);
    if (!first.data || (plan.overlap && !second.data)) return S_FALSE;

    Handle out;
    out.value = CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                            fail_if_exists ? CREATE_NEW : CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (out.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    bool keep = false;
    struct Discard {
        HANDLE file;
        bool& keep;
        ~Discard() {
            if (keep) return;
            FILE_DISPOSITION_INFO dispose{TRUE};
            SetFileInformationByHandle(file, FileDispositionInfo, &dispose, sizeof(dispose));
        }
    } discard{out.value, keep};
    if (owned_output) {
        VerificationStamp stamp;
        if (CaptureOwnedOutputStamp(out.value, stamp)) *owned_output = std::make_shared<VerificationStamp>(stamp);
    }
    // A compressed or encrypted target folder hands its attribute to new files;
    // unbuffered writes do not suit them. Nothing is lost: CopyFile2 copies.
    FILE_BASIC_INFO created{};
    if (!GetFileInformationByHandleEx(out.value, FileBasicInfo, &created, sizeof(created)) ||
        (created.FileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE)))
        return S_FALSE;
    // Named streams onto a volume without them (NTFS to FAT): CopyFile2 decides what is kept.
    DWORD target_volume = 0;
    if (!streams.empty() && (!GetVolumeInformationByHandleW(out.value, nullptr, 0, nullptr, nullptr, &target_volume, nullptr, 0) ||
                             !(target_volume & FILE_NAMED_STREAMS)))
        return S_FALSE;
    // Reserve the clusters in one run up front; the size stays 0 until written.
    FILE_ALLOCATION_INFO allocation{};
    allocation.AllocationSize.QuadPart = static_cast<LONGLONG>(total);
    if (!SetFileInformationByHandle(out.value, FileAllocationInfo, &allocation, sizeof(allocation))) {
        const DWORD error = GetLastError();
        if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) return HRESULT_FROM_WIN32(error);
    }
    // An unbuffered handle transfers whole sectors: the tail is written
    // rounded up and the size set exactly afterwards.
    FILE_STORAGE_INFO storage{};
    DWORD sector = 4096;
    if (GetFileInformationByHandleEx(out.value, FileStorageInfo, &storage, sizeof(storage)))
        sector = (std::max<DWORD>)(sector, storage.PhysicalBytesPerSectorForPerformance);
    if (chunk % sector) return S_FALSE;

    const auto aborted = [&] {
        return (control.cancel && control.cancel->load()) || (control.skip && control.skip->load());
    };
    void* buffers[2] = {first.data, second.data};
    Request read_request, write_request;
    uint64_t read_offset = 0, written = 0;
    DWORD error = ERROR_SUCCESS;
    DWORD pending_bytes = 0;     // read into buffers[current], to be written
    int current = 0;
    bool reading = false;
    const auto start_read = [&](int index) -> bool {
        reading = read_offset < total;
        if (!reading) return true;
        const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(chunk, (total - read_offset + sector - 1) / sector * sector));
        return read_request.Start(in.value, read_offset, false, buffers[index], want, error);
    };
    const auto finish_read = [&](DWORD& got) -> bool {
        got = 0;
        if (!reading) return true;
        reading = false;
        if (!read_request.Finish(in.value, got, error)) return false;
        read_offset += got;
        return true;
    };
    const auto read_next = [&](int index, DWORD& got) { return start_read(index) && finish_read(got); };
    if (!read_next(current, pending_bytes)) return HRESULT_FROM_WIN32(error);
    while (pending_bytes > 0) {
        if (aborted()) return HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
        if (control.pause && control.pause->load() && control.wait_while_paused && !control.wait_while_paused())
            return HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
        if (control.checkpoint && !control.checkpoint()) return HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
        const DWORD write_bytes = (pending_bytes + sector - 1) / sector * sector;
        DWORD next_bytes = 0;
        const int next = plan.overlap ? 1 - current : current;
        // Different disks: the next chunk is read while this one is written. The
        // read is the asynchronous request: NTFS completes a write that extends
        // the file's valid data synchronously even on an overlapped handle.
        if (plan.overlap && !start_read(next)) return HRESULT_FROM_WIN32(error);
        DWORD done = 0;
        if (!write_request.Start(out.value, written, true, buffers[current], write_bytes, error) ||
            !write_request.Finish(out.value, done, error))
            return HRESULT_FROM_WIN32(error);
        if (done != write_bytes) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        written += pending_bytes;
        if (control.report) control.report((std::min)(written, total), total);
        if (plan.overlap ? !finish_read(next_bytes) : !read_next(next, next_bytes)) return HRESULT_FROM_WIN32(error);
        current = next;
        pending_bytes = next_bytes;
    }
    if (written != total) return HRESULT_FROM_WIN32(ERROR_READ_FAULT);  // the source changed size
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(total);
    if (!SetFileInformationByHandle(out.value, FileEndOfFileInfo, &end, sizeof(end)))
        return HRESULT_FROM_WIN32(GetLastError());
    for (const auto& stream : streams) {
        if (aborted()) return HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
        if (const DWORD stream_error = CopyNamedStream(source, destination, stream))
            return HRESULT_FROM_WIN32(stream_error);
    }
    FILE_BASIC_INFO finished{};
    finished.LastWriteTime = basic.LastWriteTime;
    finished.FileAttributes = basic.FileAttributes ? basic.FileAttributes : FILE_ATTRIBUTE_NORMAL;
    if (!SetFileInformationByHandle(out.value, FileBasicInfo, &finished, sizeof(finished)))
        return HRESULT_FROM_WIN32(GetLastError());
    if (owned_output && *owned_output) {
        VerificationStamp stamp;
        if (CaptureOwnedOutputStamp(out.value, stamp)) *owned_output = std::make_shared<VerificationStamp>(stamp);
    }
    keep = true;
    return S_OK;
}

}  // namespace pulse::ops
