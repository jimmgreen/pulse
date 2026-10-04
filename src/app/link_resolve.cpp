// link_resolve.cpp — See link_resolve.h. Worker threads live for the whole
// session, so COM is initialized once per thread via a thread_local guard.
#include "link_resolve.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <winioctl.h>  // FSCTL_GET_REPARSE_POINT, MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#include <cwctype>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {

struct ThreadCom {
    ThreadCom() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                               COINIT_DISABLE_OLE1DDE); }
    ~ThreadCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
    HRESULT hr = E_FAIL;
};

bool HasLnkSuffix(const std::wstring& name) {
    if (name.size() < 4) return false;
    const std::wstring tail = name.substr(name.size() - 4);
    return _wcsicmp(tail.c_str(), L".lnk") == 0;
}

// REPARSE_DATA_BUFFER lives in the WDK headers, so only the two variants this
// file reads are laid out here. Offsets inside ReparseNames are byte offsets
// from the path buffer that follows them.
struct ReparseHeader {
    DWORD tag;
    WORD data_length;
    WORD reserved;
};
struct ReparseNames {
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
};
// A mount point keeps the path buffer directly after the names; a symlink has
// its Flags dword in between.
constexpr size_t kMountPointPathOffset = sizeof(ReparseHeader) + sizeof(ReparseNames);
constexpr size_t kSymlinkPathOffset = kMountPointPathOffset + sizeof(DWORD);
// SYMLINK_FLAG_RELATIVE (ntifs.h): the substitute name is relative to the
// directory holding the link.
constexpr DWORD kSymlinkFlagRelative = 0x00000001;

} // namespace

bool ResolveLink(const std::wstring& lnk_path, fs::DirEntry& e) {
    thread_local ThreadCom com;
    if (FAILED(com.hr)) return false;

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) return false;
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    // STGM_READ: IPersistFile::Load parses the shortcut; no target tracking.
    if (FAILED(file->Load(lnk_path.c_str(), STGM_READ))) return false;
    wchar_t raw[MAX_PATH * 4]{};
    WIN32_FIND_DATAW fd{};
    if (FAILED(link->GetPath(raw, static_cast<int>(std::size(raw)), &fd,
                             SLGP_RAWPATH)) || raw[0] == L'\0') return false;

    const std::wstring target = fs::NormalizePath(raw);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &data))
        return false;

    e.link_target = target;
    e.link_target_is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.link_target_size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                         data.nFileSizeLow;
    e.link_target_mtime = data.ftLastWriteTime;
    return true;
}

bool ResolveReparsePoint(const std::wstring& path, fs::DirEntry& e) {
    const std::wstring full = fs::NormalizePath(path);
    HANDLE handle = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD bytes = 0;
    const BOOL read = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                      buffer.data(), static_cast<DWORD>(buffer.size()),
                                      &bytes, nullptr);
    CloseHandle(handle);
    if (!read || bytes < kMountPointPathOffset) return false;

    const auto* header = reinterpret_cast<const ReparseHeader*>(buffer.data());
    e.reparse_kind = fs::KindFromTag(header->tag);
    if (!fs::IsLinkReparse(e.reparse_kind)) return false;

    const bool symlink = e.reparse_kind == fs::ReparseKind::Symlink;
    const size_t path_offset = symlink ? kSymlinkPathOffset : kMountPointPathOffset;
    const auto* names = reinterpret_cast<const ReparseNames*>(
        buffer.data() + sizeof(ReparseHeader));
    if (path_offset + names->substitute_offset + names->substitute_length > bytes)
        return false;
    const auto* path_buffer = reinterpret_cast<const wchar_t*>(buffer.data() + path_offset);
    std::wstring target(path_buffer + names->substitute_offset / sizeof(wchar_t),
                        names->substitute_length / sizeof(wchar_t));
    if (target.empty()) return false;

    const DWORD flags = symlink
        ? *reinterpret_cast<const DWORD*>(buffer.data() + kMountPointPathOffset) : 0;
    if (flags & kSymlinkFlagRelative) {
        // GetFullPathNameW collapses the "." and ".." segments of the joined
        // path; fs::NormalizePath would keep them literal behind \\?\\.
        wchar_t resolved[32768]{};
        const DWORD length = GetFullPathNameW((fs::ParentPath(full) + L"\\" + target).c_str(),
                                              static_cast<DWORD>(std::size(resolved)),
                                              resolved, nullptr);
        if (length == 0 || length >= std::size(resolved)) return false;
        target.assign(resolved, length);
    } else if (target.starts_with(L"\\??\\")) {
        target = target.substr(4);
        if (target.starts_with(L"UNC\\")) target = L"\\\\" + target.substr(4);
    }

    const std::wstring resolved_target = fs::NormalizePath(target);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(resolved_target.c_str(), GetFileExInfoStandard, &data))
        return false;

    e.link_target = resolved_target;
    e.link_target_is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.link_target_size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                         data.nFileSizeLow;
    e.link_target_mtime = data.ftLastWriteTime;
    return true;
}

void ResolveLinksInPlace(const std::wstring& parent_path,
                         std::vector<fs::DirEntry>& entries,
                         const std::function<bool()>& cancel) {
    bool any = false;
    for (const auto& e : entries) {
        if (!e.is_dir && HasLnkSuffix(e.name)) { any = true; break; }
    }
    if (!any) return;
    for (size_t i = 0; i < entries.size(); ++i) {
        if ((i & 63u) == 0 && cancel && cancel()) return;
        fs::DirEntry& e = entries[i];
        if (e.is_dir || !HasLnkSuffix(e.name)) continue;
        std::wstring full = e.full_path;
        if (full.empty()) {
            if (parent_path.empty() || fs::IsVirtualPath(parent_path)) continue;
            full = parent_path;
            if (full.back() != L'\\') full += L'\\';
            full += e.name;
        }
        ResolveLink(full, e);
    }
}

void ResolveReparsePointsInPlace(const std::wstring& parent_path,
                                 std::vector<fs::DirEntry>& entries,
                                 const std::function<bool()>& cancel) {
    bool any = false;
    for (const auto& e : entries) {
        if (e.is_reparse) { any = true; break; }
    }
    if (!any) return;
    for (size_t i = 0; i < entries.size(); ++i) {
        if ((i & 63u) == 0 && cancel && cancel()) return;
        fs::DirEntry& e = entries[i];
        if (!e.is_reparse) continue;
        std::wstring full = e.full_path;
        if (full.empty()) {
            if (parent_path.empty() || fs::IsVirtualPath(parent_path)) continue;
            full = parent_path;
            if (full.back() != L'\\') full += L'\\';
            full += e.name;
        }
        ResolveReparsePoint(full, e);
    }
}

} // namespace pulse::app
