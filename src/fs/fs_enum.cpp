// fs_enum.cpp
#include "fs_enum.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <lm.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "shlwapi.lib")

namespace pulse::fs {

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

struct NtFileFullDirInformation {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};

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
    if (g_NtCreateFile) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) throw std::runtime_error("ntdll.dll not loaded");
    g_NtCreateFile = reinterpret_cast<NtCreateFile_t>(GetProcAddress(ntdll, "NtCreateFile"));
    g_NtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFile_t>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
    if (!g_NtCreateFile || !g_NtQueryDirectoryFile)
        throw std::runtime_error("NtCreateFile / NtQueryDirectoryFile not found");
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
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    // Relative path: \\?\ requires a fully-qualified path, resolve it first.
    if (path.size() < 2 || path[1] != L':') {
        wchar_t full[32768];
        DWORD n = GetFullPathNameW(path.c_str(), (DWORD)_countof(full), full, nullptr);
        if (n > 0 && n < _countof(full)) path.assign(full, n);
    }
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    // Remove trailing backslash except for drive root.
    if (path.size() > 1 && path.back() == L'\\' && path[path.size() - 2] != L':')
        path.pop_back();
    return L"\\\\?\\" + path;
}

std::wstring ParentPath(const std::wstring& path) {
    std::wstring normalized = NormalizePath(path);
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

static void EnumerateFindFirstFileEx(const std::wstring& path, std::vector<DirEntry>& out) {
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
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        e.attrs = fd.dwFileAttributes;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.cloud_recall = (fd.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void EnumerateNtQuery(const std::wstring& path, std::vector<DirEntry>& out) {
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

    constexpr SIZE_T kBufSize = 64 * 1024;
    std::vector<BYTE> buffer(kBufSize);
    bool restart = true;
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        CloseHandle(h);
        throw std::runtime_error("CreateEvent failed");
    }

    for (;;) {
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
            WaitForSingleObject(hEvent, INFINITE);
            status = iosb.Status;
        }

        if (status == STATUS_NO_MORE_FILES) break;
        if (status != STATUS_SUCCESS) {
            CloseHandle(hEvent);
            CloseHandle(h);
            std::ostringstream oss;
            oss << "NtQueryDirectoryFile failed, status=0x" << std::hex << status;
            throw std::runtime_error(oss.str());
        }

        auto* info = reinterpret_cast<NtFileFullDirInformation*>(buffer.data());
        for (;;) {
            DirEntry e;
            e.name.assign(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (!(e.name.size() == 1 && e.name[0] == L'.') &&
                !(e.name.size() == 2 && e.name[0] == L'.' && e.name[1] == L'.')) {
                e.size = static_cast<uint64_t>(info->EndOfFile.QuadPart);
                e.mtime.dwLowDateTime = info->LastWriteTime.LowPart;
                e.mtime.dwHighDateTime = info->LastWriteTime.HighPart;
                e.attrs = info->FileAttributes;
                e.is_dir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.is_reparse = (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                e.cloud_recall = (info->FileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                out.push_back(std::move(e));
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<NtFileFullDirInformation*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }

    CloseHandle(hEvent);
    CloseHandle(h);
}

// "This PC" view (empty path): one entry per logical drive, label matches
// the sidebar ("Label (C:)" or a localized fallback). full_path is set so
// the app layer can navigate/open without joining parent+name.
static void EnumerateThisPc(std::vector<DirEntry>& out) {
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
        wchar_t volName[MAX_PATH + 1] = {};
        GetVolumeInformationW(root, volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        DirEntry e;
        e.name = std::wstring(volName[0] ? volName : L"本地磁盘") +
                 L" (" + root[0] + L":)"; // 本地磁盘
        e.full_path = NormalizePath(root);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
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

void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out) {
    out.clear();
    if (path.empty()) {
        EnumerateThisPc(out);
        return;
    }
    std::wstring normalized = NormalizePath(path);
    if (normalized.starts_with(L"\\\\?\\UNC\\")) {
        std::wstring rest = normalized.substr(8);
        while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
        if (!rest.empty() && rest.find(L'\\') == std::wstring::npos) {
            EnumerateServerShares(rest, out);
            return;
        }
    }
    try {
        EnumerateNtQuery(normalized, out);
        return;
    } catch (...) {
        // Fall back to FindFirstFileExW.
    }
    EnumerateFindFirstFileEx(normalized, out);
}

} // namespace pulse::fs
