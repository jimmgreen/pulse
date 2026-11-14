#include "setup_common.h"

#include <shlobj.h>
#include <winternl.h>

#include <algorithm>
#include <cwctype>

namespace pulse::setup {

std::wstring Win32Error(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring message = L"error " + std::to_wstring(code);
    if (text) {
        message += L": ";
        message += text;
        LocalFree(text);
    }
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
        message.pop_back();
    return message;
}

std::wstring NormalizeDir(std::wstring path) {
    auto blank = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    while (!path.empty() && blank(path.front())) path.erase(path.begin());
    while (!path.empty() && blank(path.back())) path.pop_back();
    for (auto& c : path) if (c == L'/') c = L'\\';
    // Keep "C:\" as a root; strip any other trailing separator.
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
    return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}

bool SameDirectory(const std::wstring& a, const std::wstring& b) {
    const auto x = NormalizeDir(a), y = NormalizeDir(b);
    return !x.empty() && !y.empty() && CompareStringOrdinal(x.c_str(), -1, y.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool PathInDirectories(const std::wstring& path, const std::vector<std::wstring>& dirs) {
    // An unreadable path counts as inside so an uninspectable process blocks setup.
    if (path.empty()) return true;
    for (const auto& d : dirs) {
        auto dir = NormalizeDir(d);
        if (dir.empty()) continue;
        if (dir.back() != L'\\') dir += L'\\';
        if (path.size() >= dir.size() &&
            CompareStringOrdinal(path.c_str(), static_cast<int>(dir.size()), dir.c_str(),
                                 static_cast<int>(dir.size()), TRUE) == CSTR_EQUAL)
            return true;
    }
    return false;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) out = raw;
    CoTaskMemFree(raw);
    return out;
}

std::wstring ExpandEnv(const std::wstring& text) {
    const DWORD n = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (!n) return text;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(text.c_str(), out.data(), n);
    out.resize(n - 1);
    return out;
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

RunResult RunProcess(const std::wstring& exe, const std::wstring& args, bool wait, const std::wstring& cwd) {
    RunResult result;
    std::wstring command = Quote(exe);
    if (!args.empty()) command += L" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return result;
    result.started = true;
    CloseHandle(pi.hThread);
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &result.exit_code);
    }
    CloseHandle(pi.hProcess);
    return result;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

std::wstring ParentDir(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool CreateDirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const auto parent = ParentDir(dir);
    if (!parent.empty() && parent.size() < dir.size() && parent.back() != L':') CreateDirs(parent);
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadWholeFile(const std::wstring& path, std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = data.empty() || (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
                              read == data.size());
    }
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
}

namespace {
struct CleanupHandles {
    std::vector<HANDLE> values;
    ~CleanupHandles() { for (HANDLE handle : values) CloseHandle(handle); }
};

struct CleanupHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit CleanupHandle(HANDLE handle) : value(handle) {}
    ~CleanupHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    CleanupHandle(const CleanupHandle&) = delete;
    CleanupHandle& operator=(const CleanupHandle&) = delete;
};

struct CleanupContext {
    using Open = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using Error = ULONG (WINAPI*)(NTSTATUS);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const Open open = reinterpret_cast<Open>(GetProcAddress(module, "NtCreateFile"));
    const Error error = reinterpret_cast<Error>(GetProcAddress(module, "RtlNtStatusToDosError"));
    std::vector<HANDLE> parents;
};

HANDLE OpenCleanupChild(const CleanupContext& context, HANDLE parent, const std::wstring& name,
                        DWORD access, DWORD share = FILE_SHARE_READ) {
    if (!context.open || !context.error || name.empty() || name == L"." || name == L".." ||
        name.size() > 32767 || name.find_first_of(L"\\/:") != std::wstring::npos) return INVALID_HANDLE_VALUE;
    UNICODE_STRING text{};
    text.Buffer = const_cast<PWSTR>(name.data());
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &text;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK io{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const NTSTATUS status = context.open(&handle, access | SYNCHRONIZE, &attributes, &io, nullptr, 0, share,
        1 /* FILE_OPEN */, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ | 0x00200000 /* FILE_OPEN_REPARSE_POINT */,
        nullptr, 0);
    if (status >= 0) return handle;
    SetLastError(context.error(status));
    return INVALID_HANDLE_VALUE;
}

bool SameCleanupEntry(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return (a.nFileIndexHigh || a.nFileIndexLow) && a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow;
}

void DeferRemoval(const CleanupContext& context, const std::wstring& path, DWORD error) {
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED &&
        error != ERROR_DIR_NOT_EMPTY) return;
    // Reboot removal is necessarily path-based. Queue it only while every
    // ordinary ancestor is still pinned to that name and is not a reparse point.
    for (HANDLE parent : context.parents) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(parent, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    }
    MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

void ClearCleanupReadOnly(HANDLE entry, DWORD attributes) {
    if (!(attributes & FILE_ATTRIBUTE_READONLY)) return;
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = attributes & ~FILE_ATTRIBUTE_READONLY;
    if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    SetFileInformationByHandle(entry, FileBasicInfo, &basic, sizeof(basic));
}

void RemoveTreeEntry(CleanupContext& context, HANDLE parent, const std::wstring& name,
                     const std::wstring& path, const wchar_t* keep_child) {
    constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    CleanupHandle observed(OpenCleanupChild(context, parent, name, FILE_READ_ATTRIBUTES, share_all));
    BY_HANDLE_FILE_INFORMATION before{};
    if (observed.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(observed.value, &before)) return;
    const bool plain_directory = (before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    const bool keep_directory = plain_directory && keep_child;
    const DWORD access = FILE_READ_ATTRIBUTES | (plain_directory ? FILE_LIST_DIRECTORY : 0) |
        (keep_directory ? 0 : DELETE);
    HANDLE opened = OpenCleanupChild(context, parent, name, access | FILE_WRITE_ATTRIBUTES);
    if (opened == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        opened = OpenCleanupChild(context, parent, name, access);
    CleanupHandle entry(opened);
    if (entry.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (keep_directory) return;
        // A read pin can still bind a locked file's name for the legacy reboot
        // queue. If it or its identity cannot be verified, retain the entry.
        CleanupHandle pin(OpenCleanupChild(context, parent, name, FILE_READ_ATTRIBUTES | FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE));
        BY_HANDLE_FILE_INFORMATION pinned{};
        if (pin.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(pin.value, &pinned) ||
            !SameCleanupEntry(before, pinned)) return;
        CleanupHandle metadata(OpenCleanupChild(context, parent, name, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, share_all));
        BY_HANDLE_FILE_INFORMATION writable{};
        if (metadata.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(metadata.value, &writable) &&
            SameCleanupEntry(before, writable)) ClearCleanupReadOnly(metadata.value, writable.dwFileAttributes);
        DeferRemoval(context, path, error);
        return;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(entry.value, &info) || !SameCleanupEntry(before, info)) return;
    if (plain_directory && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        context.parents.push_back(entry.value);
        // Both enumeration and single-component opens stay relative to the
        // opened object. Even an in-place junction conversion cannot redirect
        // them through a fresh lookup of the directory's absolute path.
        std::vector<uint64_t> buffer(4096);
        bool complete = true;
        for (;;) {
            if (!GetFileInformationByHandleEx(entry.value, FileIdBothDirectoryInfo, buffer.data(),
                    static_cast<DWORD>(buffer.size() * sizeof(uint64_t)))) {
                complete = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
            size_t offset = 0;
            for (;;) {
                const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const BYTE*>(buffer.data()) + offset);
                const size_t available = buffer.size() * sizeof(uint64_t) - offset;
                if (available < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) || item->FileNameLength % sizeof(wchar_t) ||
                    item->FileNameLength > available - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) { complete = false; break; }
                const std::wstring child(item->FileName, item->FileNameLength / sizeof(wchar_t));
                if (child != L"." && child != L".." && !(keep_child && EqualsNoCase(child, keep_child)))
                    RemoveTreeEntry(context, entry.value, child, path + L"\\" + child, nullptr);
                if (!item->NextEntryOffset) break;
                if (item->NextEntryOffset > available || item->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) ||
                    item->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
                    complete = false; break;
                }
                offset += item->NextEntryOffset;
            }
            if (!complete) break;
        }
        context.parents.pop_back();
        if (!complete) return;
    }
    if (keep_directory) return;
    ClearCleanupReadOnly(entry.value, info.dwFileAttributes);
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(entry.value, FileDispositionInfo, &disposition, sizeof(disposition)))
        DeferRemoval(context, path, GetLastError());
}

size_t CleanupRootLength(const std::wstring& path) {
    size_t start = 0;
    if (path.starts_with(L"\\\\?\\UNC\\")) start = 8;
    else if (path.starts_with(L"\\\\?\\"))
        return path.size() >= 7 && path[5] == L':' && path[6] == L'\\' ? 7 : 0;
    else if (path.starts_with(L"\\\\")) start = 2;
    else return path.size() >= 3 && path[1] == L':' && path[2] == L'\\' ? 3 : 0;
    const auto server = path.find(L'\\', start);
    if (server == std::wstring::npos || server == start) return 0;
    const auto share = path.find(L'\\', server + 1);
    return share == std::wstring::npos || share == server + 1 ? 0 : share + 1;
}
} // namespace

void RemoveTree(const std::wstring& dir, const wchar_t* keep_child) {
    CleanupContext context;
    if (!context.open || !context.error) return;
    const auto input = NormalizeDir(dir);
    if (input.empty()) return;
    std::wstring path(32768, L'\0');
    const DWORD length = GetFullPathNameW(input.c_str(), static_cast<DWORD>(path.size()), path.data(), nullptr);
    if (!length || length >= path.size()) return;
    path.resize(length);
    path = NormalizeDir(path);
    const size_t root = CleanupRootLength(path);
    if (!root || path.size() <= root) return;
    CleanupHandles ancestors;
    HANDLE volume = CreateFileW(path.substr(0, root).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return;
    ancestors.values.push_back(volume);
    size_t start = root;
    // Walk one component at a time from the volume/share root. A caller naming
    // Pulse\\packs must not bypass protection when Pulse itself is a junction.
    for (auto end = path.find(L'\\', start); end != std::wstring::npos; end = path.find(L'\\', start)) {
        HANDLE ancestor = OpenCleanupChild(context, ancestors.values.back(), path.substr(start, end - start),
            FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY);
        if (ancestor == INVALID_HANDLE_VALUE) return;
        ancestors.values.push_back(ancestor);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(ancestor, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        start = end + 1;
    }
    context.parents = ancestors.values;
    RemoveTreeEntry(context, ancestors.values.back(), path.substr(start), path, keep_child);
}

std::wstring Utf8ToWide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace pulse::setup
