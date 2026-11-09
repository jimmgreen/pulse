#include "../common/app_runtime_paths.h"
#include "window_jump_list.h"
#include "../common/localization.h"

// Before propkey.h, which needs the COM types; WIN32_LEAN_AND_MEAN keeps them
// out of windows.h, and targets without the app PCH get nothing else first.
#include <windows.h>
#include <objbase.h>

#include <array>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <string>
#include <thread>
#include <wrl/client.h>

namespace pulse::app {
namespace {
using Microsoft::WRL::ComPtr;

bool AddTask(IObjectCollection *tasks, const wchar_t *exe, const wchar_t *arguments, const std::wstring &title) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;
    if (FAILED(link->SetPath(exe)) || FAILED(link->SetArguments(arguments)) || FAILED(link->SetIconLocation(exe, 0)))
        return false;
    ComPtr<IPropertyStore> properties;
    if (FAILED(link.As(&properties)))
        return false;
    PROPVARIANT value{};
    if (FAILED(InitPropVariantFromString(title.c_str(), &value)))
        return false;
    const HRESULT titled = properties->SetValue(PKEY_Title, value);
    PropVariantClear(&value);
    return SUCCEEDED(titled) && SUCCEEDED(properties->Commit()) && SUCCEEDED(tasks->AddObject(link.Get()));
}
} // namespace

void WriteWindowJumpList(const std::array<std::wstring, 3> &titles) {
    const auto exe = app_runtime_paths::StableEntryPath();
    if (exe.empty()) return;
    ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
        return;
    PWSTR app_id = nullptr;
    if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&app_id))) {
        const HRESULT selected = list->SetAppID(app_id);
        CoTaskMemFree(app_id);
        if (FAILED(selected))
            return;
    }
    UINT slots = 0;
    ComPtr<IObjectArray> removed;
    if (FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed))))
        return;
    ComPtr<IObjectCollection> tasks;
    if (FAILED(
            CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks))) ||
        !AddTask(tasks.Get(), exe.c_str(), L"--new-window", titles[0]) || !AddTask(tasks.Get(), exe.c_str(), L"--new-tab", titles[1]) ||
        !AddTask(tasks.Get(), exe.c_str(), L"--this-pc", titles[2])) {
        list->AbortList();
        return;
    }
    ComPtr<IObjectArray> entries;
    if (FAILED(tasks.As(&entries)) || FAILED(list->AddUserTasks(entries.Get())) || FAILED(list->CommitList()))
        list->AbortList();
}

void UpdateWindowJumpList() {
    const std::array<std::wstring, 3> titles{l10n::Get(l10n::StringId::WindowOverviewNewWindow),
                                             l10n::Get(l10n::StringId::OpenNewTab), l10n::Get(l10n::StringId::ThisPc)};
    // Own the worker through process shutdown; Shell COM must not block the UI.
    static std::jthread worker([titles] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized))
            return;
        WriteWindowJumpList(titles);
        CoUninitialize();
    });
}
} // namespace pulse::app
