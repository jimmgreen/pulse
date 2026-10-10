#include "nt_directory_record.h"
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <thread>
#include <chrono>
// fs_enum.cpp
#include "fs_enum.h"
#include "bounded_enumeration.h"
#include "../common/localization.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <lm.h>
#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "shlwapi.lib")

namespace pulse::fs {

DWORD ReadReparseTag(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(NormalizePath(path).c_str(), FindExInfoBasic,
        &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return 0;
    FindClose(find);
    return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? data.dwReserved0 : 0;
}

using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NO_MORE_FILES = static_cast<NTSTATUS>(0x80000006L);

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct NtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using NtPioApcRoutine = VOID (NTAPI*)(PVOID ApcContext, NtIoStatusBlock* IoStatusBlock, ULONG Reserved);

using pulse::fs::NtFileFullDirInformation;

enum NtFileInformationClass : int {
    NtFileFullDirectoryInformation = 2,
};

constexpr ULONG NT_FILE_LIST_DIRECTORY = 0x0001;
constexpr ULONG NT_SYNCHRONIZE = 0x00100000;
constexpr ULONG NT_FILE_OPEN = 1;
constexpr ULONG NT_FILE_DIRECTORY_FILE = 0x00000001;
constexpr ULONG NT_OBJ_CASE_INSENSITIVE = 0x00000040;

#define NtInitializeObjectAttributes(p, n, a, r, s) { \
    (p)->Length = sizeof(NtObjectAttributes); \
    (p)->RootDirectory = r; \
    (p)->Attributes = a; \
    (p)->ObjectName = n; \
    (p)->SecurityDescriptor = s; \
    (p)->SecurityQualityOfService = NULL; \
}

using NtCreateFile_t = NTSTATUS (NTAPI*)(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    NtObjectAttributes* ObjectAttributes,
    NtIoStatusBlock* IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength);

using NtQueryDirectoryFile_t = NTSTATUS (NTAPI*)(
    HANDLE FileHandle,
    HANDLE Event,
    NtPioApcRoutine ApcRoutine,
    PVOID ApcContext,
    NtIoStatusBlock* IoStatusBlock,
    PVOID FileInformationBuffer,
    ULONG Length,
    NtFileInformationClass FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    NtUnicodeString* FileName,
    BOOLEAN RestartScan);

static NtCreateFile_t g_NtCreateFile = nullptr;
static NtQueryDirectoryFile_t g_NtQueryDirectoryFile = nullptr;

static void InitNtApi() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) throw std::runtime_error("ntdll.dll not loaded");
    g_NtCreateFile = reinterpret_cast<NtCreateFile_t>(GetProcAddress(ntdll, "NtCreateFile"));
    g_NtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFile_t>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
    if (!g_NtCreateFile || !g_NtQueryDirectoryFile)
        throw std::runtime_error("NtCreateFile / NtQueryDirectoryFile not found");
    });
}

bool IsVirtualPath(const std::wstring& path) {
    return path.starts_with(L"pulse:");
}

bool IsUncPath(const std::wstring& path) {
    return path.starts_with(L"\\\\?\\UNC\\") ||
           (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\"));
}

std::wstring NormalizePath(std::wstring path) {
    if (path.empty()) return path;
    if (IsVirtualPath(path)) return path;
    // Replace forward slashes with backslashes.
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\")) {
        // Drive roots must keep a trailing slash: \\?\E: is not a valid directory.
        if (path.size() == 6 && path[5] == L':') path += L'\\';
        return path;
    }
    // Keep the address bar's bare-drive -> root convention, but C:folder
    // still means that drive's working directory, not the active tab's path.
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    // Resolve ALL ordinary DOS/UNC inputs before introducing the extended
    // prefix, which disables Win32 dot-segment and drive-relative handling.
    // Empty is the failure sentinel for a nonempty input; admission/cache
    // callers must distinguish it from an intentional This-PC empty path.
    if (path.find(L'\0') != std::wstring::npos) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    wchar_t full[32768];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(_countof(full)), full, nullptr);
    if (n == 0) return {};
    if (n >= _countof(full)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return {};
    }
    path.assign(full, n);
    // Remove trailing backslash except for drive root.
    if (path.size() > 1 && path.back() == L'\\' && path[path.size() - 2] != L':')
        path.pop_back();
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring ParentPath(const std::wstring& path) {
    std::wstring normalized = NormalizePath(path);
    if (normalized.empty()) throw std::runtime_error("Invalid directory path");
    std::wstring_view view = normalized;
    // Strip long-path prefix for find_last_of logic.
    std::wstring_view core = view;
    if (core.starts_with(L"\\\\?\\UNC\\")) {
        core.remove_prefix(8);
    } else if (core.starts_with(L"\\\\?\\")) {
        core.remove_prefix(4);
    }
    std::wstring temp(core);
    auto pos = temp.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0) return normalized; // no parent
    temp.resize(pos);
    if (temp.size() == 2 && temp[1] == L':') temp += L'\\'; // drive root
    if (view.starts_with(L"\\\\?\\UNC\\")) return std::wstring(L"\\\\?\\UNC\\") + temp;
    if (view.starts_with(L"\\\\?\\")) return std::wstring(L"\\\\?\\") + temp;
    return temp;
}

std::wstring StripLnkSuffix(const std::wstring& name) {
    if (name.size() < 4) return name;
    if (_wcsicmp(name.c_str() + name.size() - 4, L".lnk") != 0) return name;
    return name.substr(0, name.size() - 4);
}

static void EnumerateFindFirstFileEx(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic<bool>* cancelled = nullptr) {
    std::wstring pattern = path;
    if (!pattern.ends_with(L"\\")) pattern += L"\\";
    pattern += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return;
        std::ostringstream oss;
        oss << "FindFirstFileExW failed, error=" << err;
        throw std::runtime_error(oss.str());
    }
    do {
        if (cancelled && cancelled->load()) { FindClose(h); throw EnumerationCancelled(); }
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        e.ctime = fd.ftCreationTime;
        e.atime = fd.ftLastAccessTime;
        e.attrs = fd.dwFileAttributes;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.reparse_tag = e.is_reparse ? fd.dwReserved0 : 0;
        e.cloud_recall = (fd.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    const DWORD error = GetLastError();
    FindClose(h);
    if (error != ERROR_NO_MORE_FILES) {
        out.clear();
        throw std::runtime_error("FindNextFileW failed, error=" + std::to_string(error));
    }
}

static void EnumerateNtQuery(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic<bool>* cancelled = nullptr) {
    InitNtApi();

    std::wstring target = path;
    // Convert Win32 long-path prefix to NT object prefix.
    if (target.starts_with(L"\\\\?\\")) {
        target = L"\\??\\" + target.substr(4);
    }
    if (target.ends_with(L"\\")) target.pop_back();

    NtUnicodeString us{};
    us.Buffer = const_cast<PWSTR>(target.c_str());
    us.Length = static_cast<USHORT>(target.size() * sizeof(WCHAR));
    us.MaximumLength = static_cast<USHORT>((target.size() + 1) * sizeof(WCHAR));

    NtObjectAttributes oa{};
    NtInitializeObjectAttributes(&oa, &us, NT_OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    NtIoStatusBlock iosb{};
    HANDLE h;
    NTSTATUS status = g_NtCreateFile(
        &h,
        NT_FILE_LIST_DIRECTORY | NT_SYNCHRONIZE,
        &oa,
        &iosb,
        nullptr,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NT_FILE_OPEN,
        NT_FILE_DIRECTORY_FILE,
        nullptr,
        0);

    if (status != STATUS_SUCCESS) {
        std::ostringstream oss;
        oss << "NtCreateFile failed, status=0x" << std::hex << status;
        throw std::runtime_error(oss.str());
    }

    const auto close_handle = [](void* handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_handle)> directory_owner(h, close_handle);
    constexpr SIZE_T kBufSize = 64 * 1024;
    std::vector<BYTE> buffer(kBufSize);
    bool restart = true;
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        throw std::runtime_error("CreateEvent failed");
    }

    std::unique_ptr<void, decltype(close_handle)> event_owner(hEvent, close_handle);
    for (;;) {
        if (cancelled && cancelled->load()) {
            throw EnumerationCancelled();
        }
        iosb = {};
        status = g_NtQueryDirectoryFile(
            h,
            hEvent,
            nullptr,
            nullptr,
            &iosb,
            buffer.data(),
            static_cast<ULONG>(kBufSize),
            NtFileFullDirectoryInformation,
            FALSE,
            nullptr,
            restart ? TRUE : FALSE);

        restart = false;

        if (status == STATUS_PENDING) {
            while (WaitForSingleObject(hEvent, 25) == WAIT_TIMEOUT) {
                if (cancelled && cancelled->load()) {
                    CancelIoEx(h, nullptr);
                    // The caller can retire this worker. These stack-owned NT
                    // buffers must nevertheless survive until completion.
                    WaitForSingleObject(hEvent, INFINITE);
                    break;
                }
            }
            status = iosb.Status;
        }

        if (status == STATUS_NO_MORE_FILES) break;
        if (status != STATUS_SUCCESS) {
            std::ostringstream oss;
            oss << "NtQueryDirectoryFile failed, status=0x" << std::hex << status;
            throw std::runtime_error(oss.str());
        }

        if (iosb.Information > buffer.size()) throw std::runtime_error("Invalid NT directory buffer size");
        auto* info = reinterpret_cast<NtFileFullDirInformation*>(buffer.data());
        for (;;) {
            pulse::fs::ValidateNtDirectoryRecord(info, static_cast<size_t>(iosb.Information) -
                static_cast<size_t>(reinterpret_cast<BYTE*>(info) - buffer.data()));
            DirEntry e;
            e.name.assign(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (!(e.name.size() == 1 && e.name[0] == L'.') &&
                !(e.name.size() == 2 && e.name[0] == L'.' && e.name[1] == L'.')) {
                e.size = static_cast<uint64_t>(info->EndOfFile.QuadPart);
                e.mtime.dwLowDateTime = info->LastWriteTime.LowPart;
                e.mtime.dwHighDateTime = info->LastWriteTime.HighPart;
                e.ctime.dwLowDateTime = info->CreationTime.LowPart;
                e.ctime.dwHighDateTime = info->CreationTime.HighPart;
                e.atime.dwLowDateTime = info->LastAccessTime.LowPart;
                e.atime.dwHighDateTime = info->LastAccessTime.HighPart;
                e.attrs = info->FileAttributes;
                e.is_dir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.is_reparse = (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                // FileFullDirectoryInformation returns the reparse tag in EaSize.
                e.reparse_tag = e.is_reparse ? info->EaSize : 0;
                e.cloud_recall = (info->FileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                out.push_back(std::move(e));
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<NtFileFullDirInformation*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }



}

namespace {
// Volume label and capacity are the only This PC fields that touch the media.
// A disconnected mapped drive, an empty card reader or a spun-down disk can
// stall them past the listing timeout, which failed the whole view with
// "目录不可用" while every drive still opened on its own. Each drive answers
// on its own thread within a short shared budget; a late drive is listed by
// its letter alone, and is not probed again while its earlier query is stuck.
constexpr auto kDriveMediaBudget = std::chrono::milliseconds(2000);

struct DriveMedia {
    std::wstring label;
    uint64_t total = 0;
    uint64_t free = 0;
};

struct DriveMediaProbe {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::optional<DriveMedia>, 26> answers;
};

std::array<std::atomic<bool>, 26> drive_media_busy{};

DriveMedia QueryDriveMedia(const wchar_t* root) {
    DWORD previous_mode = 0;
    const bool mode_set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX,
                                             &previous_mode) != FALSE;
    DriveMedia media;
    wchar_t label[MAX_PATH + 1] = {};
    if (GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0))
        media.label = label;
    ULARGE_INTEGER free_bytes{}, total_bytes{};
    if (GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, nullptr)) {
        media.total = total_bytes.QuadPart;
        media.free = free_bytes.QuadPart;
    }
    if (mode_set) SetThreadErrorMode(previous_mode, nullptr);
    return media;
}
} // namespace

// "This PC" view (empty path): one entry per logical drive, label matches
// the sidebar ("Label (C:)" or a localized fallback). full_path is set so
// the app layer can navigate/open without joining parent+name.
static void EnumerateThisPc(std::vector<DirEntry>& out, const std::atomic<bool>* cancelled) {
    const DWORD drives = GetLogicalDrives();
    auto probe = std::make_shared<DriveMediaProbe>();
    std::array<bool, 26> started{};
    int pending = 0;
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i)) || drive_media_busy[i].exchange(true)) continue;
        try {
            std::thread([probe, i] {
                const wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
                DriveMedia media;
                try { media = QueryDriveMedia(root); } catch (...) {}
                drive_media_busy[i] = false;
                {
                    std::lock_guard lock(probe->mutex);
                    probe->answers[i] = std::move(media);
                }
                probe->changed.notify_all();
            }).detach();
            started[i] = true;
            ++pending;
        } catch (...) {
            drive_media_busy[i] = false;
        }
    }
    std::array<std::optional<DriveMedia>, 26> answers;
    {
        std::unique_lock lock(probe->mutex);
        const auto deadline = std::chrono::steady_clock::now() + kDriveMediaBudget;
        for (;;) {
            int answered = 0;
            for (int i = 0; i < 26; ++i) answered += started[i] && probe->answers[i].has_value();
            if (answered == pending || (cancelled && cancelled->load()) ||
                std::chrono::steady_clock::now() >= deadline) break;
            probe->changed.wait_for(lock, std::chrono::milliseconds(25));
        }
        answers = probe->answers;
    }
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
        const DriveMedia* media = answers[i] ? &*answers[i] : nullptr;
        DirEntry e;
        e.name = (media && !media->label.empty() ? media->label
                                                 : std::wstring(l10n::Pick(L"本地磁盘", L"Local Disk"))) +
                 L" (" + root[0] + L":)";
        e.full_path = NormalizePath(root);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        // Local metadata only (no media access): the UI shows it as the type.
        e.drive_type = static_cast<uint8_t>(GetDriveTypeW(root));
        // Same figures as the sidebar drive rows; the tile view draws them.
        if (media) {
            e.drive_total = media->total;
            e.drive_free = media->free;
        }
        out.push_back(std::move(e));
    }
}

// Server-root view (\\server, normalized as \\?\UNC\server with no further
// separator): one entry per disk share, enumerated via NetShareEnum. Hidden
// admin shares (name ends in '$') are skipped. full_path is set so the app
// layer can navigate into a share without joining parent+name.
static void EnumerateServerShares(const std::wstring& server, std::vector<DirEntry>& out) {
    std::wstring host = L"\\\\" + server;
    BYTE* buf = nullptr;
    DWORD read = 0, total = 0;
    NET_API_STATUS status = NetShareEnum(
        const_cast<LPWSTR>(host.c_str()), 1, &buf, MAX_PREFERRED_LENGTH,
        &read, &total, nullptr);
    if (status != NERR_Success) {
        std::ostringstream oss;
        oss << "NetShareEnum failed, error=" << status;
        throw std::runtime_error(oss.str());
    }
    const auto* infos = reinterpret_cast<const SHARE_INFO_1*>(buf);
    for (DWORD i = 0; i < read; ++i) {
        const DWORD type = infos[i].shi1_type & ~(STYPE_SPECIAL | STYPE_TEMPORARY);
        if (type != STYPE_DISKTREE) continue;
        std::wstring name = infos[i].shi1_netname;
        if (name.empty() || name.back() == L'$') continue;
        DirEntry e;
        e.name = name;
        e.full_path = NormalizePath(host + L"\\" + name);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        out.push_back(std::move(e));
    }
    NetApiBufferFree(buf);
}

static void EnumerateDirectoryImpl(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic<bool>* cancelled) {
    out.clear();
    if (path.empty()) {
        EnumerateThisPc(out, cancelled);
        return;
    }
    std::wstring normalized = NormalizePath(path);
    if (normalized.empty()) throw std::runtime_error("Invalid directory path");
    if (normalized.starts_with(L"\\\\?\\UNC\\")) {
        std::wstring rest = normalized.substr(8);
        while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
        if (!rest.empty() && rest.find(L'\\') == std::wstring::npos) {
            EnumerateServerShares(rest, out);
            return;
        }
    }
    try {
        EnumerateNtQuery(normalized, out, cancelled);
        return;
    } catch (...) {
        if (cancelled && cancelled->load()) throw;
        // Fall back to FindFirstFileExW.
    }
    out.clear();
    EnumerateFindFirstFileEx(normalized, out, cancelled);
}

void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic_bool* cancelled) {
    EnumerateDirectoryImpl(path, out, cancelled);
}

namespace {
std::atomic<unsigned> active_directory_requests{0};
std::atomic<unsigned> active_remote_directory_requests{0};
struct DirectoryRequest {
    bool remote = false;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::vector<DirEntry> entries;
    std::exception_ptr error;
    ~DirectoryRequest() {
        if (remote) --active_remote_directory_requests;
        --active_directory_requests;
    }
};
}
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const EnumerationOptions& options) {
    out.clear();
    if (options.cancelled && options.cancelled()) throw EnumerationCancelled();
    const bool remote = IsUncPath(path);
    if (remote && active_remote_directory_requests.fetch_add(1) >= 16) {
        --active_remote_directory_requests;
        throw std::runtime_error("Remote directory provider retirement limit reached");
    }
    if (active_directory_requests.fetch_add(1) >= 32) {
        --active_directory_requests;
        if (remote) --active_remote_directory_requests;
        throw std::runtime_error("Directory provider retirement limit reached");
    }
    std::shared_ptr<DirectoryRequest> request;
    try { request = std::make_shared<DirectoryRequest>(); request->remote = remote; }
    catch (...) {
        --active_directory_requests;
        if (remote) --active_remote_directory_requests;
        throw;
    }
    std::thread worker([request, path] {
        try { EnumerateDirectoryImpl(path, request->entries, &request->cancelled); }
        catch (...) { request->error = std::current_exception(); }
        { std::lock_guard lock(request->mutex); request->done = true; }
        request->changed.notify_one();
    });
    const auto start = GetTickCount64();
    try {
        std::unique_lock lock(request->mutex);
        for (;;) {
            const bool cancelled = options.cancelled && options.cancelled();
            if (cancelled || GetTickCount64() - start >= options.timeout_ms) {
                request->cancelled = true;
                CancelSynchronousIo(worker.native_handle());
                worker.detach();
                throw std::runtime_error(cancelled ? "Directory enumeration cancelled" : "Directory enumeration timed out");
            }
            if (request->done) break;
            request->changed.wait_for(lock, std::chrono::milliseconds(25));
        }
        lock.unlock();
        worker.join();
    } catch (...) {
        if (worker.joinable()) {
            request->cancelled = true;
            CancelSynchronousIo(worker.native_handle());
            worker.detach();
        }
        throw;
    }
    if (request->error) std::rethrow_exception(request->error);
    out = std::move(request->entries);
}

} // namespace pulse::fs
