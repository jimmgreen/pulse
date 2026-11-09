#pragma once
#include "global_search_window.h"
#include <exdisp.h>
#include <shldisp.h>
#include <wrl/client.h>
#include <map>
#include <set>

namespace pulse {

// Read-only observations: never activate, navigate, or close a user's Shell window.
struct ExplorerRoutingSnapshot {
    std::set<HWND> frames;
    std::map<std::pair<HWND, long>, std::wstring> locations;
    unsigned unreadable_shell_entries = 0;
    bool complete = true;
    static ExplorerRoutingSnapshot Read() {
        ExplorerRoutingSnapshot result;
        EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
            wchar_t name[128]{};
            GetClassNameW(hwnd, name, ARRAYSIZE(name));
            if (wcscmp(name, L"CabinetWClass") == 0 || wcscmp(name, L"ExploreWClass") == 0)
                reinterpret_cast<ExplorerRoutingSnapshot*>(parameter)->frames.insert(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        Microsoft::WRL::ComPtr<IShellWindows> windows;
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows));
        if (FAILED(hr)) { result.complete = false; return result; }
        long count = 0;
        if (FAILED(windows->get_Count(&count))) { result.complete = false; return result; }
        for (long i = 0; i < count; ++i) {
            VARIANT index{}; index.vt = VT_I4; index.lVal = i;
            Microsoft::WRL::ComPtr<IDispatch> dispatch;
            Microsoft::WRL::ComPtr<IWebBrowser2> browser;
            if (FAILED(windows->Item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser))) {
                // ShellWindows also contains non-Explorer registrations and stale
                // entries. Completeness is established against every real frame below.
                ++result.unreadable_shell_entries; continue;
            }
            SHANDLE_PTR handle = 0;
            if (FAILED(browser->get_HWND(&handle))) { ++result.unreadable_shell_entries; continue; }
            const auto hwnd = reinterpret_cast<HWND>(handle);
            if (!result.frames.contains(hwnd)) continue;
            BSTR location = nullptr;
            if (FAILED(browser->get_LocationURL(&location))) { result.complete = false; continue; }
            // Home and other virtual folders can have an empty LocationURL.
            // Read their namespace identity rather than treating empty as a location.
            if (!location || !*location) {
                SysFreeString(location);
                location = nullptr;
                Microsoft::WRL::ComPtr<IDispatch> document;
                Microsoft::WRL::ComPtr<IShellFolderViewDual> view;
                Microsoft::WRL::ComPtr<Folder> folder;
                Microsoft::WRL::ComPtr<Folder2> folder2;
                Microsoft::WRL::ComPtr<FolderItem> item;
                if (FAILED(browser->get_Document(&document)) || !document || FAILED(document.As(&view)) ||
                    FAILED(view->get_Folder(&folder)) || !folder || FAILED(folder.As(&folder2)) ||
                    FAILED(folder2->get_Self(&item)) || !item || FAILED(item->get_Path(&location)) ||
                    !location || !*location) {
                    SysFreeString(location);
                    result.complete = false;
                    continue;
                }
            }
            long frame_index = 0;
            for (const auto& entry : result.locations) if (entry.first.first == hwnd) ++frame_index;
            result.locations[{hwnd, frame_index}] = location ? location : L"";
            SysFreeString(location);
        }
        for (const HWND frame : result.frames) {
            bool observed = false;
            for (const auto& entry : result.locations) observed |= entry.first.first == frame;
            if (!observed) result.complete = false;
        }
        return result;
    }
    bool SameAs(const ExplorerRoutingSnapshot& before) const {
        return complete && before.complete && frames == before.frames && locations == before.locations;
    }
};
}
