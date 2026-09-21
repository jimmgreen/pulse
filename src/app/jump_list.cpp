#include "jump_list.h"

#include "../common/localization.h"

#include <windows.h>
#include <objbase.h>
#include <propkey.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {

std::wstring ModulePath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD chars = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return chars == 0 ? std::wstring{} : std::wstring(buffer, chars);
}

// "D:\Work\Project" -> "Project", so a pinned row reads like the folder it opens
// instead of like our executable.
std::wstring FolderName(const std::wstring& folder) {
    std::wstring value = folder;
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    const size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? value : value.substr(slash + 1);
}

std::wstring Quoted(const std::wstring& text) {
    std::wstring quoted = L"\"";
    for (const wchar_t c : text) {
        if (c == L'"') quoted += L'\\';
        quoted += c;
    }
    quoted += L'"';
    return quoted;
}

// One entry: our executable, launched with `arguments`, titled `title`.
ComPtr<IShellLinkW> MakeShellLink(const std::wstring& title, const std::wstring& tooltip,
                                  const std::wstring& arguments) {
    const std::wstring exe = ModulePath();
    if (exe.empty()) return {};
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) {
        return {};
    }
    link->SetPath(exe.c_str());
    link->SetArguments(arguments.c_str());
    if (!tooltip.empty()) link->SetDescription(tooltip.c_str());
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link.As(&store))) {
        // The shell shows this as the row text; the store copies the value.
        PROPVARIANT value{};
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<wchar_t*>(title.c_str());
        if (SUCCEEDED(store->SetValue(PKEY_Title, value))) store->Commit();
    }
    return link;
}

ComPtr<IObjectCollection> NewCollection() {
    ComPtr<IObjectCollection> collection;
    CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&collection));
    return collection;
}

} // namespace

void RefreshJumpList(const std::vector<std::wstring>& pinned_folders) {
    ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&list)))) {
        return;
    }
    // The shell decides how many rows it keeps; anything past that is dropped
    // rather than making the whole list fail.
    UINT max_slots = 0;
    ComPtr<IObjectArray> removed;
    if (FAILED(list->BeginList(&max_slots, IID_PPV_ARGS(&removed)))) return;

    if (ComPtr<IObjectCollection> pinned = NewCollection()) {
        UINT added = 0;
        for (const std::wstring& folder : pinned_folders) {
            if (added >= max_slots) break;
            if (ComPtr<IShellLinkW> link = MakeShellLink(
                    FolderName(folder), folder, L"--new-window " + Quoted(folder))) {
                if (SUCCEEDED(pinned->AddObject(link.Get()))) ++added;
            }
        }
        if (added > 0) {
            list->AppendCategory(l10n::Get(l10n::StringId::JumpListPinned).c_str(),
                                 pinned.Get());
        }
    }

    if (ComPtr<IObjectCollection> tasks = NewCollection()) {
        if (ComPtr<IShellLinkW> task = MakeShellLink(
                l10n::Get(l10n::StringId::TrayNewWindow), std::wstring{}, L"--new-window")) {
            if (SUCCEEDED(tasks->AddObject(task.Get()))) list->AddUserTasks(tasks.Get());
        }
    }
    list->CommitList();
}

} // namespace pulse::app
