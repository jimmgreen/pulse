// link_resolve.cpp — See link_resolve.h. Worker threads live for the whole
// session, so COM is initialized once per thread via a thread_local guard.
#include "link_resolve.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <cwctype>

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

} // namespace pulse::app
