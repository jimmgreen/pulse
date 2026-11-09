#include "setup_shortcut.h"
#include "../common/app_identity.h"
#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <filesystem>
#include <cstdio>
#include <string>

namespace {
struct Link {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    IPropertyStore* properties = nullptr;
    Link() {
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
            link->QueryInterface(IID_PPV_ARGS(&file));
            link->QueryInterface(IID_PPV_ARGS(&properties));
        }
    }
    ~Link() { if (properties) properties->Release(); if (file) file->Release(); if (link) link->Release(); }
    bool Load(const std::filesystem::path& path) { return file && properties && SUCCEEDED(file->Load(path.c_str(), STGM_READ)); }
    std::wstring Args() { wchar_t value[1024]{}; link->GetArguments(value, ARRAYSIZE(value)); return value; }
    std::wstring Id() {
        PROPVARIANT value{};
        const HRESULT hr = properties->GetValue(PKEY_AppUserModel_ID, &value);
        const std::wstring result = SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : L"";
        PropVariantClear(&value); return result;
    }
};
}
int wmain() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const auto fixture = std::filesystem::path(executable).parent_path() /
        (L"shortcut-identity-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(fixture)) { CoUninitialize(); return 2; }
    int failures = 0;
    auto check = [&](bool passed, const char* name) { std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name); failures += !passed; };
    const auto target = (fixture / L"pulse.exe").wstring();
    const auto workdir = fixture.wstring();
    std::wstring error;
    const auto created = fixture / L"new.lnk";
    bool saved = pulse::setup::EnsureShortcut(created.wstring(), target, L"", workdir, L"Pulse", error);
    { Link read; check(saved && read.Load(created) && read.Id() == pulse::app_identity::kId && read.Args() == L"--activate", "new application shortcut persists activation argument and shared identity"); }
    const auto legacy = fixture / L"legacy.lnk";
    {
        Link old;
        if (old.file && old.properties) {
            old.link->SetPath(target.c_str()); old.link->SetArguments(L"");
            old.link->SetDescription(L"My customized label"); old.link->SetWorkingDirectory(L"C:\\Windows");
            old.link->SetIconLocation(L"C:\\Windows\\System32\\shell32.dll", 17);
            old.link->SetHotkey(MAKEWORD('P', HOTKEYF_CONTROL | HOTKEYF_ALT));
            saved = SUCCEEDED(old.file->Save(legacy.c_str(), TRUE));
        } else saved = false;
    }
    saved = saved && pulse::setup::EnsureShortcut(legacy.wstring(), target, L"", workdir, L"replacement", error);
    {
        Link read; bool ok = saved && read.Load(legacy) && read.Id() == pulse::app_identity::kId && read.Args() == L"--activate";
        wchar_t description[256]{}, directory[MAX_PATH]{}, icon[MAX_PATH]{}; int index = 0; WORD hotkey = 0;
        if (ok) {
            read.link->GetDescription(description, ARRAYSIZE(description)); read.link->GetWorkingDirectory(directory, ARRAYSIZE(directory));
            read.link->GetIconLocation(icon, ARRAYSIZE(icon), &index); read.link->GetHotkey(&hotkey);
        }
        check(ok && std::wstring(description) == L"My customized label" && std::wstring(directory) == L"C:\\Windows" &&
            std::wstring(icon) == L"C:\\Windows\\System32\\shell32.dll" && index == 17 && hotkey == MAKEWORD('P', HOTKEYF_CONTROL | HOTKEYF_ALT),
            "same-target legacy shortcut gains identity and preserves customized fields");
    }
    const auto identified = fixture / L"identified-empty-args.lnk";
    {
        Link old;
        saved = old.file && old.properties;
        if (saved) {
            old.link->SetPath(target.c_str()); old.link->SetArguments(L"");
            old.link->SetDescription(L"Keep this description");
            saved = SUCCEEDED(pulse::app_identity::SetShortcut(old.properties)) && SUCCEEDED(old.file->Save(identified.c_str(), TRUE));
        }
    }
    saved = saved && pulse::setup::EnsureShortcut(identified.wstring(), target, L"", workdir, L"replace", error);
    {
        Link read; wchar_t description[256]{};
        const bool ok = saved && read.Load(identified) && read.Id() == pulse::app_identity::kId && read.Args() == L"--activate";
        if (ok) read.link->GetDescription(description, ARRAYSIZE(description));
        check(ok && std::wstring(description) == L"Keep this description", "correct legacy identity still migrates empty arguments without resetting properties");
    }
    const FILETIME sentinel{12345678, 30000000};
    HANDLE handle = CreateFileW(legacy.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    bool stamped = handle != INVALID_HANDLE_VALUE && SetFileTime(handle, nullptr, nullptr, &sentinel);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    saved = pulse::setup::EnsureShortcut(legacy.wstring(), target, L"", workdir, L"replacement", error);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    check(stamped && saved && GetFileAttributesExW(legacy.c_str(), GetFileExInfoStandard, &attributes) &&
        CompareFileTime(&sentinel, &attributes.ftLastWriteTime) == 0, "correct identity leaves existing shortcut file untouched");
    const auto uninstall = fixture / L"uninstall.lnk";
    saved = pulse::setup::EnsureShortcut(uninstall.wstring(), (fixture / L"uninstall.exe").wstring(), L"", workdir, L"", error);
    { Link read; check(saved && read.Load(uninstall) && read.Id().empty(), "uninstaller shortcut does not receive application identity"); }
    const auto utility = fixture / L"utility.lnk";
    saved = pulse::setup::EnsureShortcut(utility.wstring(), target, L"--seed-shell-verbs", workdir, L"", error);
    saved = saved && pulse::setup::EnsureShortcut(utility.wstring(), target, L"", workdir, L"", error);
    { Link read; check(saved && read.Load(utility) && read.Id().empty() && read.Args() == L"--seed-shell-verbs", "utility invocation does not receive application identity"); }
    const auto new_window = fixture / L"new-window.lnk";
    saved = pulse::setup::EnsureShortcut(new_window.wstring(), target, L"--new-window", workdir, L"", error);
    saved = saved && pulse::setup::EnsureShortcut(new_window.wstring(), target, L"", workdir, L"", error);
    { Link read; check(saved && read.Load(new_window) && read.Id().empty() && read.Args() == L"--new-window", "explicit new-window invocation remains unchanged"); }
    for (const auto& path : {created, legacy, identified, uninstall, utility, new_window}) std::filesystem::remove(path);
    std::filesystem::remove(fixture);
    CoUninitialize();
    return failures ? 1 : 0;
}
