#include "setup_shortcut.h"

#include "setup_common.h"
#include "../common/app_identity.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <filesystem>


namespace pulse::setup {
namespace {

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

HRESULT ReadAppIdentity(IShellLinkW* link, bool& matches) {
    Com<IPropertyStore> properties;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&properties));
    if (FAILED(hr)) return hr;
    PROPVARIANT value{};
    hr = properties->GetValue(PKEY_AppUserModel_ID, &value);
    matches = SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal &&
        wcscmp(value.pwszVal, app_identity::kId) == 0;
    PropVariantClear(&value);
    return hr;
}

HRESULT SetAppIdentity(IShellLinkW* link) {
    Com<IPropertyStore> properties;
    const HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&properties));
    return FAILED(hr) ? hr : app_identity::SetShortcut(properties.p);
}

}  // namespace

bool EnsureShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args,
                    const std::wstring& workdir, const std::wstring& description, std::wstring& error) {
    const bool application = (args.empty() || args == L"--activate") && EqualsNoCase(
        std::filesystem::path(target).filename().wstring(), L"pulse.exe");
    const std::wstring effective_args = application ? L"--activate" : args;
    Com<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) { error = L"IShellLink: " + Win32Error(hr); return false; }
    Com<IPersistFile> file;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (FAILED(hr)) { error = L"IPersistFile: " + Win32Error(hr); return false; }

    const bool loaded = GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES &&
        SUCCEEDED(file.p->Load(lnk.c_str(), STGM_READ));
    if (loaded) {
        wchar_t existing[MAX_PATH]{}, existing_args[1024]{};
        const bool same_target = SUCCEEDED(link->GetPath(existing, MAX_PATH, nullptr, SLGP_RAWPATH)) &&
            SUCCEEDED(link->GetArguments(existing_args, 1024)) &&
            CompareStringOrdinal(ExpandEnv(existing).c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL;
        // Only migrate ordinary activation links, not a user's explicit tool/new-window entry.
        if (same_target && application && *existing_args && std::wstring(existing_args) != L"--activate") return true;
        if (same_target && (effective_args == existing_args || (application && !*existing_args))) {
            if (!application) return true;
            bool matches = false;
            hr = ReadAppIdentity(link.p, matches);
            if (FAILED(hr)) { error = L"Shortcut identity: " + Win32Error(hr); return false; }
            if (matches && effective_args == existing_args) return true;
            hr = file.p->Load(lnk.c_str(), STGM_READWRITE);
            if (SUCCEEDED(hr)) hr = link->SetArguments(effective_args.c_str());
            if (SUCCEEDED(hr) && !matches) hr = SetAppIdentity(link.p);
            if (FAILED(hr)) { error = L"Shortcut identity: " + Win32Error(hr); return false; }
            // Upgrade activation arguments and identity; retain customized icon, description,
            // working directory and other persisted shortcut properties.
            hr = file.p->Save(lnk.c_str(), TRUE);
            if (FAILED(hr)) { error = L"Save " + lnk + L": " + Win32Error(hr); return false; }
            return true;
        }
    }

    if (loaded && application) {
        hr = file.p->Load(lnk.c_str(), STGM_READWRITE);
        if (FAILED(hr)) { error = L"Load " + lnk + L": " + Win32Error(hr); return false; }
    }
    CreateDirs(ParentDir(lnk));
    link->SetPath(target.c_str());
    link->SetArguments(effective_args.c_str());
    link->SetWorkingDirectory(workdir.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(target.c_str(), 0);
    if (application) {
        hr = SetAppIdentity(link.p);
        if (FAILED(hr)) { error = L"Shortcut identity: " + Win32Error(hr); return false; }
    }
    hr = file.p->Save(lnk.c_str(), TRUE);
    if (FAILED(hr)) { error = L"Save " + lnk + L": " + Win32Error(hr); return false; }
    return true;
}

}  // namespace pulse::setup
