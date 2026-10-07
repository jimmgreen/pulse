// shell_items.cpp — see shell_items.h.
#include "shell_items.h"
#include "../common/current_user_security.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cwctype>

namespace pulse::shell {

namespace {

constexpr std::wstring_view kPrefix = L"pulse:shell:";

std::wstring NameOf(IShellFolder* parent, PCUITEMID_CHILD id, SHGDNF kind) {
    STRRET strret{};
    if (FAILED(parent->GetDisplayNameOf(id, kind, &strret))) return {};
    wchar_t buffer[1024]{};
    if (FAILED(StrRetToBufW(&strret, id, buffer, ARRAYSIZE(buffer)))) return {};
    return buffer;
}

// "C:\" - a drive root, which the UI already lists from GetLogicalDrives().
// Matching the whole string matters: a cloud-drive folder also lives under a
// drive letter ("C:\Users\...\WPSDrive"), and treating that as a duplicate
// would hide exactly the entries this change is about.
bool IsDriveRoot(const std::wstring& parsing_name) {
    return parsing_name.size() == 3 && parsing_name[1] == L':' &&
           (parsing_name[2] == L'\\' || parsing_name[2] == L'/') &&
           iswalpha(parsing_name[0]);
}

bool IsFilesystemFolder(IShellFolder* folder, PCUITEMID_CHILD id, bool& is_dir) {
    SFGAOF attrs = SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STREAM;
    if (FAILED(folder->GetAttributesOf(1, &id, &attrs))) return false;
    is_dir = (attrs & SFGAO_FOLDER) != 0;
    return (attrs & SFGAO_FILESYSTEM) != 0;
}

void Collect(IShellFolder* folder, std::vector<ipc::ShellItem>& out) {
    IEnumIDList* ids = nullptr;
    if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &ids)) || !ids)
        return;

    LPITEMIDLIST id = nullptr;
    while (ids->Next(1, &id, nullptr) == S_OK) {
        ipc::ShellItem item;
        item.display_name = NameOf(folder, id, SHGDN_NORMAL);
        item.parsing_name = NameOf(folder, id, SHGDN_FORPARSING);

        bool is_dir = false;
        item.has_filesystem_path = IsFilesystemFolder(folder, id, is_dir);
        item.is_dir = is_dir;

        if (!item.parsing_name.empty()) {
            if (item.has_filesystem_path) {
                // Capacity is only meaningful for a filesystem folder; the UI
                // shows it on the This PC row the way it does for drives.
                ULARGE_INTEGER free_bytes{}, total_bytes{};
                if (GetDiskFreeSpaceExW(item.parsing_name.c_str(), &free_bytes, &total_bytes,
                                        nullptr)) {
                    item.total = total_bytes.QuadPart;
                    item.free = free_bytes.QuadPart;
                }
            }
            out.push_back(std::move(item));
        }
        CoTaskMemFree(id);
    }
    ids->Release();
}

// The shell folder behind This PC.
IShellFolder* BindComputerFolder() {
    PIDLIST_ABSOLUTE computer = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &computer)) || !computer)
        return nullptr;

    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) {
        CoTaskMemFree(computer);
        return nullptr;
    }
    IShellFolder* folder = nullptr;
    desktop->BindToObject(ILFindLastID(computer), nullptr, IID_PPV_ARGS(&folder));
    desktop->Release();
    CoTaskMemFree(computer);
    return folder;
}

} // namespace

bool IsShellPath(const std::wstring& path) {
    return path.starts_with(kPrefix);
}

std::wstring ParsingNameOfPath(const std::wstring& path) {
    if (!path.starts_with(kPrefix)) return {};
    return path.substr(kPrefix.size());
}

std::wstring MakeShellPath(const std::wstring& parsing_name) {
    std::wstring path(kPrefix);
    path.append(parsing_name);
    return path;
}

std::vector<ipc::ShellItem> EnumerateThisPcExtras() {
    std::vector<ipc::ShellItem> all;
    IShellFolder* folder = BindComputerFolder();
    if (folder) {
        Collect(folder, all);
        folder->Release();
    }

    std::vector<ipc::ShellItem> out;
    out.reserve(all.size());
    for (auto& item : all) {
        if (IsDriveRoot(item.parsing_name)) continue;   // already a sidebar drive row
        out.push_back(std::move(item));
    }
    return out;
}

bool EnumerateShellFolder(const std::wstring& parsing_name, std::vector<ipc::ShellItem>& out) {
    out.clear();
    if (parsing_name.empty()) return false;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(parsing_name.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return false;

    // Binding a bare filesystem folder can fail on some paths, so fall back to
    // the shell item interface before giving up.
    IShellFolder* folder = nullptr;
    HRESULT hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(hr) || !folder) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item))) && item) {
            item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder));
            item->Release();
        }
    }
    CoTaskMemFree(pidl);

    if (!folder) return false;
    Collect(folder, out);
    folder->Release();
    return true;
}

} // namespace pulse::shell
